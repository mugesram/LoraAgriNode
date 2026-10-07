#!/bin/sh
# Flash over ST-Link (SWD), clear the STM32L4 PEMPTY flag (set by erasing page 0,
# which would make the chip boot the ROM bootloader), then reset into the app.
set -e
cd "$(dirname "$0")/.."
ok=0
for w in 1 2 3; do
  if st-flash write build/boardtest.bin 0x08000000 2>&1 | grep -E "verified|ERROR" | tee /dev/stderr | grep -q verified; then ok=1; break; fi
  echo "write failed, retrying..."; sleep 1
done
[ $ok = 1 ] || { echo "FLASH WRITE FAILED"; exit 1; }
sleep 1
for try in 1 2 3; do
  st-util -n -p 4242 >/dev/null 2>&1 &
  sleep 2
  out=$(gdb-multiarch -q -batch build/boardtest.elf \
    -ex "target extended-remote :4242" -ex "monitor halt" \
    -ex "set *(unsigned int*)0x40022010 = 0x20000" \
    -ex "p/x *(unsigned int*)0x40022010" \
    -ex "set *(unsigned int*)0xE000ED0C = 0x05FA0004" 2>&1 || true)
  pkill st-util || true
  sleep 1
  if echo "$out" | grep -q '= 0x0$'; then echo "reset into application"; exit 0; fi
done
echo "WARNING: could not clear PEMPTY; power-cycle the board to start the app"
