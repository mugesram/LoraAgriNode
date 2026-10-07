#!/usr/bin/env python3
"""Write firmware to the logger over USB, no ST-Link needed.

Sends the DFU command to the running logger firmware (USB serial cafe:4012),
or the 'b' key to the old board-test firmware (cafe:4011), waits for the ROM
bootloader (0483:df11) and runs dfu_flash.py.

If no firmware is running, put the board in bootloader mode by hand: hold
BOOT (SW2), tap RESET (SW1), release BOOT. Then run this again.

Usage: flash.py build/logger.bin
"""
import subprocess
import sys
import time
from pathlib import Path

import serial
import serial.tools.list_ports
import usb.core

HERE = Path(__file__).resolve().parent


def find(vid, pid):
    for p in serial.tools.list_ports.comports():
        if (p.vid, p.pid) == (vid, pid):
            return p.device
    return None


def in_dfu():
    return usb.core.find(idVendor=0x0483, idProduct=0xDF11) is not None


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    if not in_dfu():
        for (vid, pid, cmd, name) in ((0xCAFE, 0x4012, b"DFU\n", "logger firmware"), (0xCAFE, 0x4011, b"b", "board-test firmware")):
            port = find(vid, pid)
            if port:
                print(f"{name} on {port}: asking for bootloader mode")
                try:
                    with serial.Serial(port, 115200, timeout=1) as s:
                        s.write(cmd)
                        time.sleep(0.3)
                except (serial.SerialException, OSError):
                    pass  # the port disappears when the board resets
                break
        else:
            sys.exit("No logger found on USB. Hold BOOT (SW2), tap RESET (SW1), release BOOT, and run again.")
        for _ in range(50):
            if in_dfu():
                break
            time.sleep(0.2)
        else:
            sys.exit("The board did not appear in bootloader mode (0483:df11).")
    sys.exit(subprocess.call([sys.executable, str(HERE / "dfu_flash.py"), sys.argv[1]]))


if __name__ == "__main__":
    main()
