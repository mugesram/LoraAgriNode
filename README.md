# entc-sensornode-1: STM32L432 LoRa Sensor Logger

Everything for the custom STM32L432KCU6 board (`entc-sensornode-1`) lives here.

```
node (this board) --LoRa 866 MHz--> gateway (XIAO ESP32S3) --USB--> laptop server (main store)
node (this board) --USB---------------------------------------------> laptop server
```

| Folder | What |
|---|---|
| `datasheets/` | MCU, flash, SX1262, Wio module, board schematic |
| `firmware_flow/` | `embedded_device_plan.md` (handwritten plan), `firmware_workflow_plan.md` (design and phases) |
| `board_notes/` | Bring-up history, pin map, LoRa test setup |
| `code/` | Node firmware (STM32CubeMX layout: `Core`, `Drivers`, `Middlewares`, `App`) |
| `gateway/` | Gateway firmware for the XIAO ESP32S3 + Wio-SX1262 kit (PlatformIO) |
| `server/` | Laptop server and web page |
| `results/` | Timing and power measurements go here |

## Status (2026-10-07)

All code is written and has run end to end on the hardware (2026-10-07): node on its own supply, LoRa to the gateway, laptop server, web page. Not yet measured: sleep current on battery and the timing and power tables (phase 7).

| Part | Checked how |
|---|---|
| Data log and settings (`datalog.c`, `config.c`) | PC test against a simulated flash: ring wrap, restart, power cut, delete, format (`make test`) |
| Server and web page | PC test with a fake gateway and fake logger; page viewed in Chrome |
| Record format, firmware vs server | Server test decodes bytes produced by the firmware code |
| Node firmware as a whole | Run on the board: USB commands, flash, sensor, LoRa send, Stop 2 wake-up, time sync, firmware update |
| Gateway firmware | Run on the XIAO ESP32S3: receives, acknowledges with time, feeds the server |

## Build and flash

Node:
```
cd code
make            # build/logger.bin
make test       # storage tests on the PC
make flash      # asks the running firmware for bootloader mode, then writes over USB
```
If nothing answers on USB: hold BOOT (SW2), tap RESET (SW1), release BOOT, then `make flash`.

Gateway:
```
cd gateway
~/lora_esp32_rx/.venv/bin/pio run -t upload --upload-port /dev/ttyACM0
```

Server:
```
server/run.sh   # then open http://localhost:8080
```

## First run on the board

Do these in order. Each step has one thing to look for.

1. **Flash the node** with `make flash`. The first start erases the old test data from the flash chip, which takes about 12 seconds.
   Look for: `lsusb` shows `cafe:4012 ENTC entc-sensornode-1`.
2. **Start the server** and open the page, tab "Logger on USB".
   Look for: Flash chip OK, Radio chip OK. Press "Set time from this PC".
3. **Press "Read sensor now".**
   Look for: a moisture value and "works".
4. **Flash the gateway** and leave it plugged into the laptop.
   Look for: "gateway connected" at the top of the page.
5. **Unplug the node from USB and run it from the battery**, then wait two collection periods (default 60 s each; the first reading is sent on the second wake-up). The node does not collect while it is on USB.
   Look for: the logger appears on the "LoRa data" tab and readings keep arriving. Measure the sleep current between wake-ups.

If a step fails, the page's Status box shows which part (flash, radio, LoRa replies) and the board notes list the known hardware faults.

## Things that are different from a usual board

- **No crystal.** The clock runs on the internal LSI oscillator. The logger corrects its rate each time the time is set over USB or by a LoRa reply.
- **No VBUS sense.** The firmware looks for a PC every 2 seconds by testing the D+ line. A plain charger is not seen as a PC, so the logger keeps sleeping on a charger.
- **No collection on USB.** While a PC is connected the node only answers commands; "Run one cycle" on the page still takes one reading. After USB is switched off the next reading comes one full period later.
- **Watchdog.** The MCU wakes every 2 seconds for a few microseconds to feed the watchdog and check for USB. Collection happens only at the set period.
- **Ping before data.** Each cycle the node first sends a 4-byte ping. Data goes out only if the gateway answers. The node times that answer and waits twice as long for every later reply; until it has a measurement it uses the fixed setting (300 ms), and after 3 missed replies in a row it goes back to the fixed value.
- **Gateway replies only while the server runs.** If the laptop server is stopped, the node gets no reply, keeps its readings, and sends them later.
- **One sensor connector.** J1 (PA0) is shorted on this board, so both sensor types use J2. Setting "Sensor" chooses moisture (0) or DS18B20 (1).
