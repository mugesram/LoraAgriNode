#!/usr/bin/env python3
"""Read the timing log of a power-supply test build and write it as CSV.

Plug the board into the laptop after the test and run:
    tlog_dump.py            one CSV per record kind in ../../results/
    tlog_dump.py --clear    the same, then erase the log on the board

The log is read through the laptop server if it is running (it holds the
serial port), otherwise straight from the board.
"""
import argparse
import json
import sys
import time
import urllib.request
from pathlib import Path

import serial
import serial.tools.list_ports

RESULTS = Path(__file__).resolve().parents[2] / "results"
SERVER = "http://localhost:8080"
KINDS = {
    2: ("flash", "errors", ["read_n", "read_avg_us", "read_min_us", "read_max_us",
                            "prog_n", "prog_avg_us", "prog_min_us", "prog_max_us",
                            "erase_n", "erase_avg_us", "erase_max_us", "sleep_us", "wake_us", "rec16_write_us"]),
    3: ("sensor_settle", "flags", ["vdda_mV", "off_mV", "final_mV", "within_10pct_ms", "within_5pct_ms",
                                   "within_2pct_ms", "within_1pct_ms", "peak_mV", "peak_at_ms", "ripple_mV"]),
    4: ("cycle", "saved_calc", ["flash_wake_us", "radio_wake_us", "lora_send_us", "lora_tx_us", "lora_ack_us",
                                "sensor_wait_us", "sensor_read_us", "flash_save_us", "cycle_us", "records_sent",
                                "moisture_mV", "vdda_mV", "reply_timeout_ms", "reply_ms"]),
    5: ("sensor_trace", "flags", ["mV_at_%dms" % t for t in (1, 2, 5, 10, 20, 50, 100, 200, 300, 500, 750, 1000, 1500, 1999)]),
}


def via_server(cmd):
    req = urllib.request.Request(SERVER + "/api/node/command", data=json.dumps({"cmd": cmd}).encode())
    with urllib.request.urlopen(req, timeout=150) as r:
        out = json.load(r)
    if not out.get("ok"):
        sys.exit(f"{cmd}: {out.get('error')}")
    return out["lines"]


def via_serial(cmd):
    port = next((p.device for p in serial.tools.list_ports.comports() if (p.vid, p.pid) == (0xCAFE, 0x4012)), None)
    if not port:
        sys.exit("No board found on USB.")
    lines, buf = [], b""
    with serial.Serial(port, 115200, timeout=0.2) as s:
        time.sleep(0.2)
        s.reset_input_buffer()
        s.write((cmd + "\n").encode())
        deadline = time.time() + 30
        while time.time() < deadline:
            chunk = s.read(4096)
            if chunk:
                buf += chunk
                deadline = time.time() + 10
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("ascii", "replace").strip()
                if line == "OK":
                    return lines
                if line.startswith("ERR"):
                    sys.exit(f"{cmd}: {line}")
                if line:
                    lines.append(line)
    sys.exit(f"{cmd}: no answer from the board")


def command(cmd):
    try:
        return via_server(cmd)
    except OSError:
        return via_serial(cmd)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--clear", action="store_true", help="erase the log on the board after saving it")
    ap.add_argument("--out", default=str(RESULTS))
    a = ap.parse_args()

    rows = {}
    for line in command("TLOG?"):
        p = line.split(",")
        if p[0] == "L" and len(p) == 19:
            rows.setdefault(int(p[1]), []).append(p[2:])
    if not rows:
        print("The timing log on the board is empty.")
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y-%m-%d_%H%M%S")
    for kind, recs in sorted(rows.items()):
        name, flag_name, cols = KINDS.get(kind, (f"kind{kind}", "flags", [f"v{i}" for i in range(14)]))
        path = out / f"powertest_{name}_{stamp}.csv"
        with open(path, "w") as f:
            f.write(",".join([flag_name, "seq", "node_time_s"] + cols) + "\n")
            for r in recs:
                f.write(",".join(r[:3 + len(cols)]) + "\n")
        print(f"{len(recs)} records -> {path}")
    if a.clear:
        command("TCLEAR")
        print("Timing log on the board erased.")


if __name__ == "__main__":
    main()
