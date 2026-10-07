#!/usr/bin/env python3
"""Read the firmware's RAM log (g_log) over SWD and print it."""
import subprocess, struct, sys
elf = "build/boardtest.elf"
nm = subprocess.run(["arm-none-eabi-nm", elf], capture_output=True, text=True).stdout
addr = {l.split()[2]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}
base = addr["g_log_done"]
subprocess.run(["st-flash", "--hot-plug", "read", "build/log.bin", hex(base), hex(8 + 6144)],
               capture_output=True)
d = open("build/log.bin", "rb").read()
done, ln = struct.unpack_from("<II", d, 0)
print("self-test finished" if done == 0x600DF00D else "self-test NOT finished", f"({ln} bytes)")
print(d[addr["g_log"] - base:addr["g_log"] - base + ln].decode(errors="replace").replace("\r", ""))
