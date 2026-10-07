#!/usr/bin/env python3
"""Test of server.py without hardware.

A fake gateway and a fake logger are attached through pseudo-terminals. The
LoRa packet below was produced by the firmware's own datalog.c (three
records), so this also checks that the firmware and the server agree on the
record format and CRC.

Run:  ../../.venv/bin/python test_server.py
"""
import json
import os
import pty
import subprocess
import sys
import tempfile
import threading
import time
import tty
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = 8099
# seq 1: t=1791000000 valid, 2980 mV, 26.50 C, 3294 mV
# seq 2: t=1791000060 valid, no moisture, -5.25 C, 3301 mV
# seq 3: t=42 clock not set, 1500 mV, no temperature, 3300 mV
RECORDS_HEX = ("01000000C07DC06AA40B5A0ADE0C1F6C" "02000000FC7DC06AFFFFF3FDE50C1F4B" "030000002A000000DC05FF7FE40C0F15")
PACKET_HEX = "A501" + "3412" + "03" + "01" + "0700" + RECORDS_HEX  # device 0x1234, 3 records, 7 remaining


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        raise SystemExit(1)


def get(path):
    with urllib.request.urlopen(f"http://127.0.0.1:{PORT}{path}", timeout=30) as r:
        return json.loads(r.read())


def post(path, obj):
    req = urllib.request.Request(f"http://127.0.0.1:{PORT}{path}", data=json.dumps(obj).encode(), method="POST")
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read())


def open_pty():
    master, slave = pty.openpty()
    tty.setraw(slave)
    return master, os.ttyname(slave)


class FakeNode(threading.Thread):
    """Answers like usb_cli.c."""

    def __init__(self, fd):
        super().__init__(daemon=True)
        self.fd = fd
        self.cfg = {"period_s": 60, "tx_dbm": 10}
        self.time_set = None
        self.erased = None

    def reply(self, *lines):
        os.write(self.fd, ("".join(l + "\r\n" for l in lines)).encode())

    def run(self):
        buf = b""
        while True:
            buf += os.read(self.fd, 1024)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.handle(line.decode().strip())

    def handle(self, line):
        if line == "ID?":
            self.reply("id=4660", "fw=1.0.0", "proto=1", "OK")
        elif line == "STATUS?":
            self.reply("time=1791000100", "time_valid=1", "records=3", "unsent=1", "OK")
        elif line == "CFG?":
            self.reply("period_s=%d 5 86400" % self.cfg["period_s"], "tx_dbm=%d -9 22" % self.cfg["tx_dbm"], "OK")
        elif line.startswith("CFG "):
            k, v = line[4:].split("=")
            if k in self.cfg and int(v) >= 5:
                self.cfg[k] = int(v)
                self.reply("OK")
            else:
                self.reply("ERR unknown key or value out of range")
        elif line.startswith("TIME="):
            self.time_set = int(line[5:])
            self.reply("OK")
        elif line.startswith("DATA?"):
            self.reply("R,1,1791000000,1,2980,2650,3294,1", "T,2,1791000050,7", "R,3,1791000060,1,65535,-525,3301,0", "END 3", "OK")
        elif line.startswith("ERASE "):
            self.erased = line[6:]
            self.reply("deleted=2", "OK")
        else:
            self.reply("ERR unknown command")


def main():
    gw_fd, gw_port = open_pty()
    node_fd, node_port = open_pty()
    node = FakeNode(node_fd)
    node.start()
    tmp = tempfile.mkdtemp()
    srv = subprocess.Popen([sys.executable, str(HERE.parent / "server.py"), "--http-port", str(PORT),
                            "--gateway-port", gw_port, "--node-port", node_port, "--db", f"{tmp}/t.db"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        # the server must send the time to the gateway
        os.set_blocking(gw_fd, False)
        got, t0 = b"", time.time()
        while b"\n" not in got and time.time() - t0 < 10:
            try:
                got += os.read(gw_fd, 100)
            except BlockingIOError:
                time.sleep(0.1)
        check(got.startswith(b"T ") and abs(int(got.split()[1]) - time.time()) < 5, "server sends the time to the gateway")

        os.write(gw_fd, f"GW HELLO v1 b2b\nGW PKT rssi=-45.5 snr=12.2 hex={PACKET_HEX}\n".encode())
        os.write(gw_fd, f"GW PKT rssi=-45.5 snr=12.2 hex={PACKET_HEX}\n".encode())          # same packet again
        os.write(gw_fd, b"GW PKT rssi=-90.0 snr=1.0 hex=4C34333220233132\n")                 # not a logger packet
        bad = PACKET_HEX[:20] + "FF" + PACKET_HEX[22:]                                       # one damaged record
        os.write(gw_fd, f"GW PKT rssi=-45.5 snr=12.2 hex={bad}\n".encode())
        time.sleep(1.5)

        st = get("/api/gateway")
        check(st["gateway"]["connected"], "gateway shown as connected")
        check(len(st["devices"]) == 1 and st["devices"][0]["dev"] == 0x1234, "device 0x1234 listed")
        check(st["devices"][0]["records"] == 3 and st["devices"][0]["remaining"] == 7, "3 records stored once, 7 remaining")
        check(st["node_port"] == node_port, "logger on USB detected")
        os.write(gw_fd, b"GW PKT rssi=-60.0 snr=9.0 hex=A5037856\n")                          # ping from node 0x5678
        time.sleep(0.8)
        devs = {d["dev"]: d for d in get("/api/gateway")["devices"]}
        check(0x5678 in devs and devs[0x5678]["rssi"] == -60.0 and devs[0x5678]["records"] == 0, "ping lists the node, stores nothing")

        rows = get("/api/readings")
        check(len(rows) == 3, "3 readings returned")
        by_seq = {r["seq"]: r for r in rows}
        check(by_seq[1]["moisture_mV"] == 2980 and by_seq[1]["temp_c"] == 26.5 and by_seq[1]["vdda_mV"] == 3294
              and by_seq[1]["time"] == 1791000000, "record 1 decoded")
        check(by_seq[2]["moisture_mV"] is None and by_seq[2]["temp_c"] == -5.25, "record 2: no moisture, negative temperature")
        check(by_seq[3]["time_valid"] == 0 and abs(by_seq[3]["time"] - time.time()) < 30 and by_seq[3]["temp_c"] is None,
              "record 3: clock not set, stored at arrival time")
        check(len(get("/api/readings?dev=1")) == 0 and len(get("/api/readings?from=1791000050&to=1791000070")) == 1, "filters")
        with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/api/export.csv?dev=4660") as r:
            check(len(r.read().decode().strip().splitlines()) == 4, "CSV export has header and 3 rows")
        with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/") as r:
            check(b"LoRa Sensor Logger" in r.read(), "web page served")

        # logger on USB
        s = get("/api/node/status")
        check(s["ok"] and s["id"]["id"] == "4660" and s["status"]["unsent"] == "1", "node status")
        c = get("/api/node/config")
        check(c["ok"] and c["config"][0] == {"key": "period_s", "value": 60, "min": 5, "max": 86400}, "node settings read")
        check(post("/api/node/config", {"key": "period_s", "value": 300})["ok"] and node.cfg["period_s"] == 300, "setting changed")
        r = post("/api/node/config", {"key": "period_s", "value": 1})
        check(not r["ok"] and "out of range" in r["error"], "bad setting refused with the logger's reason")
        check(post("/api/node/time", {})["ok"] and abs(node.time_set - time.time()) < 5, "time set from the PC")
        d = get("/api/node/data?save=1")
        check(d["ok"] and len(d["records"]) == 2 and d["time_changes"] == [{"seq": 2, "new": 1791000050, "old": 7}], "data download")
        check(d["records"][1]["moisture_mV"] is None and d["records"][1]["temp_c"] == -5.25 and d["records"][1]["sent"] == 0, "download decoded")
        check(d["saved_new"] == 1, "download saved: 1 already known from LoRa, 1 new")
        check(post("/api/node/erase", {"before": 1791000000})["result"]["deleted"] == "2" and node.erased == "BEFORE 1791000000", "delete by date")
        check(post("/api/node/erase", {"all": True})["ok"] and node.erased == "ALL", "delete all")
        print("ALL SERVER TESTS PASSED")
    finally:
        srv.terminate()
        out = srv.communicate(timeout=5)[0]
        if "Traceback" in out:
            print(out)


if __name__ == "__main__":
    main()
