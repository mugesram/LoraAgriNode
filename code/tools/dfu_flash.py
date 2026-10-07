#!/usr/bin/env python3
"""Minimal DfuSe flasher for the STM32 ROM USB bootloader (0483:df11).

Usage: dfu_flash.py firmware.bin [--addr 0x08000000] [--no-leave]
Erases only the pages needed, writes, verifies by reading back, then starts the app.
"""
import argparse
import sys
import time

import usb.core
import usb.util

VID, PID = 0x0483, 0xDF11
DNLOAD, UPLOAD, GETSTATUS, CLRSTATUS, ABORT = 1, 2, 3, 4, 6
ST_IDLE, ST_DNBUSY, ST_DNIDLE, ST_MANIFEST, ST_UPIDLE, ST_ERROR = 2, 4, 5, 7, 9, 10
PAGE = 2048


class Dfu:
    def __init__(self):
        self.dev = usb.core.find(idVendor=VID, idProduct=PID)
        if self.dev is None:
            sys.exit("No STM32 in DFU mode found (hold BOOT/SW2, tap RESET/SW1).")
        self.dev.set_configuration()
        usb.util.claim_interface(self.dev, 0)
        self.dev.set_interface_altsetting(0, 0)
        self.xfer = 2048
        cfg = self.dev.get_active_configuration()
        extra = bytes(cfg[(0, 0)].extra_descriptors or b"")
        i = 0
        while i + 1 < len(extra):
            ln, typ = extra[i], extra[i + 1]
            if typ == 0x21 and ln >= 7:  # DFU functional descriptor
                self.xfer = extra[i + 5] | (extra[i + 6] << 8)
            i += max(ln, 1)

    def status(self):
        for attempt in range(20):
            try:
                s = self.dev.ctrl_transfer(0xA1, GETSTATUS, 0, 0, 6, timeout=5000)
                break
            except usb.core.USBError as e:
                if e.errno != 32 or attempt == 19:  # retry only on stall while busy
                    raise
                time.sleep(0.05)
        poll = s[1] | (s[2] << 8) | (s[3] << 16)
        return s[0], s[4], poll

    def wait(self):
        while True:
            st, state, poll = self.status()
            if st != 0:
                raise RuntimeError(f"DFU error status {st} (state {state})")
            if state != ST_DNBUSY:
                return state
            time.sleep(poll / 1000.0)

    def idle(self):
        st, state, _ = self.status()
        if state == ST_ERROR:
            self.dev.ctrl_transfer(0x21, CLRSTATUS, 0, 0, None)
        elif state != ST_IDLE:
            self.dev.ctrl_transfer(0x21, ABORT, 0, 0, None)
        st, state, _ = self.status()
        if state != ST_IDLE:
            raise RuntimeError(f"cannot reach dfuIDLE (state {state})")

    def cmd(self, code, addr):
        self.dev.ctrl_transfer(0x21, DNLOAD, 0, 0, bytes([code]) + addr.to_bytes(4, "little"))
        self.wait()

    def erase(self, addr):
        self.cmd(0x41, addr)

    def set_addr(self, addr):
        self.cmd(0x21, addr)

    def write(self, addr, data):
        self.idle()
        self.set_addr(addr)
        for n in range(0, len(data), self.xfer):
            self.dev.ctrl_transfer(0x21, DNLOAD, 2 + n // self.xfer, 0, data[n:n + self.xfer])
            self.wait()
            done = min(n + self.xfer, len(data))
            print(f"\r  write {done}/{len(data)} bytes", end="", flush=True)
        print()

    def read(self, addr, length):
        self.idle()
        self.set_addr(addr)
        self.idle()
        out = bytearray()
        blk = 2
        while len(out) < length:
            out += bytes(self.dev.ctrl_transfer(0xA1, UPLOAD, blk, 0, self.xfer, timeout=5000))
            blk += 1
        self.idle()
        return bytes(out[:length])

    def leave(self, addr):
        self.idle()
        self.set_addr(addr)
        self.dev.ctrl_transfer(0x21, DNLOAD, 0, 0, None)
        try:
            self.status()
        except usb.core.USBError:
            pass  # device detaches and jumps to the application


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bin")
    ap.add_argument("--addr", type=lambda s: int(s, 0), default=0x08000000)
    ap.add_argument("--no-leave", action="store_true")
    a = ap.parse_args()

    data = open(a.bin, "rb").read()
    data += b"\xff" * (-len(data) % 8)  # L4 programs in 64-bit double words
    d = Dfu()
    print(f"STM32 DFU found, transfer size {d.xfer}")

    d.idle()
    pages = range(a.addr, a.addr + len(data), PAGE)
    for i, p in enumerate(pages):
        d.erase(p)
        print(f"\r  erase page {i + 1}/{len(pages)}", end="", flush=True)
    print()
    d.write(a.addr, data)

    print("  verifying...", end="", flush=True)
    back = d.read(a.addr, len(data))
    if back != data:
        bad = next(i for i in range(len(data)) if back[i] != data[i])
        sys.exit(f" FAILED at 0x{a.addr + bad:08X}")
    print(" OK")

    if not a.no_leave:
        d.leave(a.addr)
        print("Started application.")


if __name__ == "__main__":
    main()
