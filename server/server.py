#!/usr/bin/env python3
"""Laptop server for the STM32L432 LoRa sensor logger.

This is the main store of the system:
  - reads the LoRa gateway (XIAO ESP32S3) on USB serial, sends it the time
    every few seconds, and saves every received record in SQLite;
  - talks to a logger node plugged in by USB (status, settings, data
    download, delete, firmware update);
  - serves the web page at http://localhost:8080 .

Run:  ../.venv/bin/python server.py
"""
import argparse
import json
import sqlite3
import struct
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import serial
import serial.tools.list_ports

HERE = Path(__file__).resolve().parent
WEB_DIR = HERE / "web"
DB_PATH = HERE / "data" / "logger.db"
DFU_TOOL = HERE.parent / "code" / "tools" / "dfu_flash.py"
RESULTS_DIR = HERE.parent / "results"

GATEWAY_USB = (0x303A, 0x1001)  # ESP32-S3 USB serial/JTAG
NODE_USB = (0xCAFE, 0x4012)     # logger firmware CDC port

LINK_MAGIC, LINK_TYPE_DATA, LINK_TYPE_PING, LINK_TYPE_TEST, LINK_HDR_LEN, REC_LEN = 0xA5, 1, 3, 5, 8, 16
F_UNSENT, F_LIVE, F_TIME_VALID, F_TYPE_MASK = 0x01, 0x02, 0x10, 0x60
NO_MOISTURE, NO_TEMP = 0xFFFF, 0x7FFF
HEARTBEAT_S = 5


def log(*args):
    print(time.strftime("%H:%M:%S"), *args, flush=True)


def find_port(vid_pid):
    for p in serial.tools.list_ports.comports():
        if (p.vid, p.pid) == vid_pid:
            return p.device
    return None


# ------------------------------------------------------------------ records
def rec_crc(rec: bytes) -> int:
    """CRC-8 of a 16-byte record, same as rec_crc() in datalog.c."""
    crc = 0xA5
    for i in range(15):
        b = rec[i] & 0xF0 if i == 14 else rec[i]
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def parse_record(rec: bytes):
    """16 raw bytes -> dict, or None if the CRC is wrong or it is not a reading."""
    if len(rec) != REC_LEN or rec_crc(rec) != rec[15]:
        return None
    seq, t, moisture, temp, vdda, flags, _ = struct.unpack("<IIHhHBB", rec)
    if flags & F_TYPE_MASK:
        return None
    return {
        "seq": seq,
        "time": t,
        "time_valid": 1 if flags & F_TIME_VALID else 0,
        "moisture_mV": None if moisture == NO_MOISTURE else moisture,
        "temp_c": None if temp == NO_TEMP else temp / 100.0,
        "vdda_mV": vdda,
    }


def parse_packet(pkt: bytes):
    """LoRa data packet -> (device id, remaining, [records]) or None."""
    if len(pkt) < LINK_HDR_LEN + REC_LEN or pkt[0] != LINK_MAGIC or pkt[1] != LINK_TYPE_DATA:
        return None
    dev, count, _flags, remaining = struct.unpack_from("<HBBH", pkt, 2)
    if count == 0 or len(pkt) != LINK_HDR_LEN + count * REC_LEN:
        return None
    recs = []
    for i in range(count):
        r = parse_record(pkt[LINK_HDR_LEN + i * REC_LEN: LINK_HDR_LEN + (i + 1) * REC_LEN])
        if r:
            recs.append(r)
    return dev, remaining, recs


# ------------------------------------------------------- power-supply test
LORA_TEST_COLS = ["radio_wake_us", "ping_setup_us", "ping_air_us", "pong_setup_us", "pong_wait_us",
                  "data_setup_us", "data_air_us", "ack_setup_us", "ack_wait_us"]


def save_lora_test_row(pkt: bytes, rssi, snr, folder: Path = None):
    """Packet of the 'lora' test build: the node's own timing of its previous
    round, in 10 us steps (0xFFFF = did not happen). One CSV row per packet."""
    dev, seq = struct.unpack_from("<HH", pkt, 2)
    if seq == 0:
        return None   # first packet after power-up: no round before it
    vals = struct.unpack_from("<9H", pkt, 6)
    folder = folder or RESULTS_DIR
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / time.strftime("powertest_lora_%Y-%m-%d.csv")
    new = not path.exists()
    with open(path, "a") as f:
        if new:
            f.write("pc_time,node,round,rssi_dbm,snr_db," + ",".join(LORA_TEST_COLS) + "\n")
        f.write(",".join([time.strftime("%Y-%m-%d %H:%M:%S"), str(dev), str(seq - 1), str(rssi), str(snr)]
                         + ["" if v == 0xFFFF else str(v * 10) for v in vals]) + "\n")
    return path


# ----------------------------------------------------------------- database
class Store:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.lock = threading.Lock()
        self.db = sqlite3.connect(str(path), check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS readings (
              dev INTEGER NOT NULL, seq INTEGER NOT NULL, time INTEGER NOT NULL,
              time_valid INTEGER NOT NULL, moisture_mV INTEGER, temp_c REAL, vdda_mV INTEGER,
              rssi REAL, snr REAL, source TEXT NOT NULL, received_at INTEGER NOT NULL,
              PRIMARY KEY (dev, seq, time));
            CREATE INDEX IF NOT EXISTS readings_time ON readings (dev, time);
            CREATE TABLE IF NOT EXISTS devices (
              dev INTEGER PRIMARY KEY, last_seen INTEGER, remaining INTEGER, rssi REAL, snr REAL);
        """)

    def add(self, dev, recs, source, rssi=None, snr=None, remaining=None):
        """Insert records; rows already stored are left alone. Returns the number of new rows."""
        now = int(time.time())
        new = 0
        with self.lock:
            for r in recs:
                # a record taken before the node clock was set is placed at its arrival time
                t = r["time"] if r["time_valid"] else now
                if not r["time_valid"] and self.db.execute(
                        "SELECT 1 FROM readings WHERE dev = ? AND seq = ? AND time_valid = 0", (dev, r["seq"])).fetchone():
                    continue  # the same record sent again after a lost ACK
                cur = self.db.execute(
                    "INSERT OR IGNORE INTO readings VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                    (dev, r["seq"], t, r["time_valid"], r["moisture_mV"], r["temp_c"], r["vdda_mV"],
                     rssi, snr, source, now))
                new += cur.rowcount
            if source == "lora":
                self.db.execute("INSERT OR REPLACE INTO devices VALUES (?,?,?,?,?)", (dev, now, remaining, rssi, snr))
            self.db.commit()
        return new

    def seen(self, dev, rssi, snr):
        """A ping was heard: the node is alive, nothing new to store."""
        now = int(time.time())
        with self.lock:
            cur = self.db.execute("UPDATE devices SET last_seen = ?, rssi = ?, snr = ? WHERE dev = ?", (now, rssi, snr, dev))
            if cur.rowcount == 0:
                self.db.execute("INSERT INTO devices VALUES (?,?,?,?,?)", (dev, now, None, rssi, snr))
            self.db.commit()

    def readings(self, dev=None, t_from=0, t_to=2 ** 32, limit=5000):
        q = "SELECT * FROM readings WHERE time BETWEEN ? AND ?"
        args = [t_from, t_to]
        if dev is not None:
            q += " AND dev = ?"
            args.append(dev)
        q += " ORDER BY time DESC, seq DESC LIMIT ?"
        args.append(limit)
        with self.lock:
            rows = [dict(r) for r in self.db.execute(q, args)]
        rows.reverse()
        return rows

    def devices(self):
        with self.lock:
            return [dict(r) for r in self.db.execute(
                "SELECT d.*, (SELECT COUNT(*) FROM readings r WHERE r.dev = d.dev) AS records,"
                " (SELECT MAX(time) FROM readings r WHERE r.dev = d.dev) AS last_time"
                " FROM devices d ORDER BY d.dev")]


# ------------------------------------------------------------------ gateway
class Gateway(threading.Thread):
    """Keeps the gateway serial port open, feeds it the time, stores packets."""

    def __init__(self, store: Store, port=None):
        super().__init__(daemon=True)
        self.store = store
        self.fixed_port = port
        self.status = {"connected": False, "port": None, "packets": 0, "records_new": 0,
                       "last_packet": None, "alive": None, "last_line": None}

    def run(self):
        while True:
            port = self.fixed_port or find_port(GATEWAY_USB)
            if not port:
                time.sleep(2)
                continue
            try:
                with serial.Serial(port, 115200, timeout=0.5) as ser:
                    log(f"gateway connected on {port}")
                    self.status.update(connected=True, port=port)
                    self.serve(ser)
            except (serial.SerialException, OSError) as e:
                log(f"gateway lost: {e}")
            self.status.update(connected=False, port=None)
            time.sleep(2)

    def serve(self, ser):
        last_beat = 0.0
        buf = b""
        while True:
            if time.time() - last_beat >= HEARTBEAT_S:
                ser.write(f"T {int(time.time())}\n".encode())
                last_beat = time.time()
            buf += ser.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.handle(line.decode("ascii", "replace").strip())

    def handle(self, line):
        if not line.startswith("GW "):
            return
        self.status["last_line"] = line
        parts = line.split()
        kind = parts[1] if len(parts) > 1 else ""
        fields = dict(p.split("=", 1) for p in parts[2:] if "=" in p)
        if kind == "PKT":
            try:
                pkt = bytes.fromhex(fields["hex"])
                rssi, snr = float(fields["rssi"]), float(fields["snr"])
            except (KeyError, ValueError):
                return
            if len(pkt) == 4 and pkt[0] == LINK_MAGIC and pkt[1] == LINK_TYPE_PING:
                self.store.seen(struct.unpack_from("<H", pkt, 2)[0], rssi, snr)
                self.status["pings"] = self.status.get("pings", 0) + 1
                return
            if len(pkt) == 24 and pkt[0] == LINK_MAGIC and pkt[1] == LINK_TYPE_TEST:
                save_lora_test_row(pkt, rssi, snr)
                self.store.seen(struct.unpack_from("<H", pkt, 2)[0], rssi, snr)
                self.status["test_packets"] = self.status.get("test_packets", 0) + 1
                return
            parsed = parse_packet(pkt)
            if not parsed:
                log(f"gateway: packet not from a logger ({len(pkt)} bytes)")
                return
            dev, remaining, recs = parsed
            new = self.store.add(dev, recs, "lora", rssi, snr, remaining)
            self.status["packets"] += 1
            self.status["records_new"] += new
            self.status["last_packet"] = int(time.time())
            log(f"node {dev}: {len(recs)} records ({new} new), {remaining} still to come, rssi {rssi} snr {snr}")
        elif kind == "ALIVE":
            self.status["alive"] = fields
        elif kind in ("HELLO", "ERROR", "NOACK"):
            log("gateway:", line[3:])


# --------------------------------------------------------------- node (USB)
class NodeError(Exception):
    pass


class Node:
    """A logger node plugged in by USB: sends one command, collects the reply lines."""

    def __init__(self, port=None):
        self.fixed_port = port
        self.lock = threading.Lock()
        self.ser = None

    def port(self):
        return self.fixed_port or find_port(NODE_USB)

    def _open(self):
        if self.ser and self.ser.is_open:
            return
        port = self.port()
        if not port:
            raise NodeError("no logger found on USB")
        self.ser = serial.Serial(port, 115200, timeout=0.2)
        time.sleep(0.2)
        self.ser.reset_input_buffer()

    def _close(self):
        try:
            if self.ser:
                self.ser.close()
        except (serial.SerialException, OSError):
            pass
        self.ser = None

    def command(self, cmd, first_timeout=20.0, idle_timeout=5.0):
        """Returns the reply lines without the final OK. Raises NodeError on ERR or time-out."""
        with self.lock:
            try:
                self._open()
                self.ser.write((cmd + "\n").encode())
                lines, buf = [], b""
                deadline = time.time() + first_timeout
                while time.time() < deadline:
                    chunk = self.ser.read(4096)
                    if chunk:
                        buf += chunk
                        deadline = time.time() + idle_timeout
                    while b"\n" in buf:
                        raw, buf = buf.split(b"\n", 1)
                        line = raw.decode("ascii", "replace").strip()
                        if line == "OK":
                            return lines
                        if line.startswith("ERR"):
                            raise NodeError(line[4:] or "error")
                        if line:
                            lines.append(line)
                raise NodeError("no answer from the logger")
            except (serial.SerialException, OSError) as e:
                self._close()
                raise NodeError(f"USB port error: {e}")

    def kv(self, cmd, **kw):
        out = {}
        for line in self.command(cmd, **kw):
            if "=" in line:
                k, v = line.split("=", 1)
                out[k] = v
        return out

    def data(self, t_from=0, t_to=0):
        """Download stored records. Returns (records, time_changes)."""
        recs, changes = [], []
        for line in self.command(f"DATA? {t_from} {t_to}", first_timeout=30):
            p = line.split(",")
            if p[0] == "R" and len(p) == 8:
                moisture, temp = int(p[4]), int(p[5])
                recs.append({"seq": int(p[1]), "time": int(p[2]), "time_valid": int(p[3]),
                             "moisture_mV": None if moisture == NO_MOISTURE else moisture,
                             "temp_c": None if temp == NO_TEMP else temp / 100.0,
                             "vdda_mV": int(p[6]), "sent": int(p[7])})
            elif p[0] == "T" and len(p) == 4:
                changes.append({"seq": int(p[1]), "new": int(p[2]), "old": int(p[3])})
        return recs, changes

    def flash(self, image: bytes):
        """Firmware update: DFU command, then the DfuSe tool."""
        path = HERE / "data" / "upload.bin"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(image)
        out = []
        try:
            self.command("DFU", first_timeout=5)
            out.append("logger restarted in bootloader mode")
        except NodeError as e:
            out.append(f"DFU command: {e} (continuing: the board may already be in bootloader mode)")
        with self.lock:
            self._close()
        time.sleep(3)
        r = subprocess.run([sys.executable, str(DFU_TOOL), str(path)], capture_output=True, text=True, timeout=180)
        out += (r.stdout + r.stderr).strip().splitlines()
        return r.returncode == 0, out


# --------------------------------------------------------------------- HTTP
class Handler(BaseHTTPRequestHandler):
    store: Store = None
    gateway: Gateway = None
    node: Node = None

    def log_message(self, *args):
        pass

    def send_json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_file(self, path: Path):
        types = {".html": "text/html", ".js": "text/javascript", ".css": "text/css"}
        if not path.is_file() or WEB_DIR not in path.resolve().parents:
            self.send_error(404)
            return
        body = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", types.get(path.suffix, "application/octet-stream") + "; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def node_call(self, fn):
        try:
            self.send_json({"ok": True, **fn()})
        except NodeError as e:
            self.send_json({"ok": False, "error": str(e)})

    def do_GET(self):
        url = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        if url.path == "/":
            self.send_file(WEB_DIR / "index.html")
        elif url.path == "/api/gateway":
            self.send_json({"gateway": self.gateway.status, "devices": self.store.devices(),
                            "node_port": self.node.port(), "server_time": int(time.time())})
        elif url.path == "/api/readings":
            dev = int(q["dev"]) if q.get("dev") else None
            self.send_json(self.store.readings(dev, int(q.get("from", 0)), int(q.get("to", 2 ** 32)),
                                               int(q.get("limit", 5000))))
        elif url.path == "/api/export.csv":
            dev = int(q["dev"]) if q.get("dev") else None
            rows = self.store.readings(dev, int(q.get("from", 0)), int(q.get("to", 2 ** 32)), 10 ** 7)
            cols = ["dev", "seq", "time", "time_valid", "moisture_mV", "temp_c", "vdda_mV", "rssi", "snr", "source"]
            text = ",".join(cols) + "\n" + "".join(
                ",".join("" if r[c] is None else str(r[c]) for c in cols) + "\n" for r in rows)
            body = text.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/csv")
            self.send_header("Content-Disposition", "attachment; filename=readings.csv")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif url.path == "/api/node/status":
            self.node_call(lambda: {"id": self.node.kv("ID?"), "status": self.node.kv("STATUS?")})
        elif url.path == "/api/node/sensor":
            self.node_call(lambda: {"sensor": self.node.kv("SENSOR?")})
        elif url.path == "/api/node/timing":
            self.node_call(lambda: {"timing": self.node.kv("TIMING?")})
        elif url.path == "/api/node/config":
            def cfg():
                out = []
                for line in self.node.command("CFG?"):
                    key, rest = line.split("=", 1)
                    value, lo, hi = rest.split()
                    out.append({"key": key, "value": int(value), "min": int(lo), "max": int(hi)})
                return {"config": out}
            self.node_call(cfg)
        elif url.path == "/api/node/data":
            def data():
                recs, changes = self.node.data(int(q.get("from", 0)), int(q.get("to", 0)))
                new = 0
                if q.get("save") == "1" and recs:
                    dev = int(self.node.kv("ID?")["id"])
                    new = self.store.add(dev, recs, "usb")
                return {"records": recs, "time_changes": changes, "saved_new": new}
            self.node_call(data)
        else:
            self.send_file(WEB_DIR / url.path.lstrip("/"))

    def do_POST(self):
        url = urlparse(self.path)
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""
        if url.path == "/api/node/flash":
            if len(body) < 1024 or len(body) > 256 * 1024:
                self.send_json({"ok": False, "error": "not a firmware image (size)"})
                return
            ok, out = self.node.flash(body)
            self.send_json({"ok": ok, "log": out})
            return
        try:
            req = json.loads(body or b"{}")
        except ValueError:
            self.send_json({"ok": False, "error": "bad request"}, 400)
            return
        if url.path == "/api/node/time":
            self.node_call(lambda: {"lines": self.node.command(f"TIME={int(time.time())}")})
        elif url.path == "/api/node/config":
            self.node_call(lambda: {"lines": self.node.command(f"CFG {req['key']}={int(req['value'])}")})
        elif url.path == "/api/node/erase":
            cmd = "ERASE ALL" if req.get("all") else f"ERASE BEFORE {int(req['before'])}"
            self.node_call(lambda: {"result": self.node.kv(cmd, first_timeout=120)})
        elif url.path == "/api/node/cycle":
            self.node_call(lambda: {"lines": self.node.command("CYCLE")})
        elif url.path == "/api/node/command":
            self.node_call(lambda: {"lines": self.node.command(str(req.get("cmd", ""))[:90], first_timeout=120)})
        else:
            self.send_error(404)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--gateway-port", help="serial port of the gateway (default: found by USB id)")
    ap.add_argument("--node-port", help="serial port of a logger on USB (default: found by USB id)")
    ap.add_argument("--db", default=str(DB_PATH))
    args = ap.parse_args()

    store = Store(Path(args.db))
    gateway = Gateway(store, args.gateway_port)
    gateway.start()
    Handler.store, Handler.gateway, Handler.node = store, gateway, Node(args.node_port)
    log(f"web page: http://localhost:{args.http_port}  (database {args.db})")
    ThreadingHTTPServer(("127.0.0.1", args.http_port), Handler).serve_forever()


if __name__ == "__main__":
    main()
