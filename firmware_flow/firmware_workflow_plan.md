# Firmware Workflow Plan: STM32L432 LoRa Sensor Logger

Written 2026-10-07. Built from `embedded_device_plan.md` (the handwritten notes), the board schematic and the datasheets in `../datasheets/`, and the bring-up results in `../board_notes/`.

Page numbers like (P1) point back to the page of the handwritten notes that a step comes from.

## Implementation status (2026-10-07)

Phases 0 to 6 and the watchdog part of phase 8 are written in `../code/`, `../gateway/` and `../server/`. The code builds and its PC tests pass, but it has not run on the board yet. Phase 7 (timing and power measurements) and the long soak test need the hardware. See `../README.md` for the first-run steps.

What changed from this plan while writing the code:

| Plan | As built | Why |
|---|---|---|
| Web page talks to the logger directly (Web Serial) | Web page talks to the laptop server, and the server talks to the logger over USB | The laptop server is the main store. One page then shows both LoRa data and the USB logger, and it works in any browser. |
| Gateway keeps time by itself | Server sends the time to the gateway every 5 s; the gateway puts it in each ACK | The ESP32 only receives; the laptop is the time source. |
| Gateway always ACKs | Gateway ACKs only while the server is running | Otherwise readings would be marked "sent" with nobody storing them. |
| USB detected by an interrupt on D+ | D+ is tested every 2 s: driven high, released, read back | A PC pulls D+ firmly low, so this gives clean logic levels; the pull-up method sat near the input threshold. |
| One long sleep per period | Sleep in 2 s pieces | The watchdog cannot be stopped in Stop 2 and must be fed. The extra current is negligible. |
| RTC prescaler tuned to the LSI | Prescaler fixed; a rate factor corrects the time in software | Simpler, and the rate is re-learned from every time sync. |
| Firmware update by WebUSB | Server runs the existing `dfu_flash.py` after sending `DFU` | Reuses the tool already proven on this board. |
| - | First start on a chip without valid settings erases the data ring | The board-test firmware left random data in the flash. |

---

## 1. What the device does

The device sleeps almost all the time. A timer wakes it at the collection frequency. Each wake it sends old unsent readings over LoRa, takes one new reading, saves it to flash, and sleeps again. When a PC is plugged in by USB it switches to USB mode, where a web page reads the data, changes settings, and updates the firmware.

Two modes:

| Mode | Entered by | What runs |
|---|---|---|
| Normal (P1, P4) | RTC wake-up timer | One collection cycle, then Stop 2 sleep |
| USB (P2, P3, P5) | USB plug-in | Command handler for the web page; no timed collection cycle while on USB, the period starts again when USB is switched off |

---

## 2. Hardware facts that shape the firmware

These come from the schematic and datasheets. Each one forces a design choice below.

| Fact | Effect on firmware |
|---|---|
| PC14/PC15 not connected, so no 32.768 kHz crystal | RTC must run on LSI. LSI is 31.04 to 32.96 kHz at 30 °C (about ±3 %), which is up to 43 minutes of drift per day if left alone. Time must be calibrated and re-synced (section 7). |
| VBUS goes only to Q2 and D1, not to an MCU pin | No direct "USB plugged" signal. USB detection needs a trick on PA11/PA12 (section 8), to be proven on the bench. |
| No LED | All feedback is over USB or LoRa. |
| No battery-voltage divider | Firmware cannot read battery level. Run-time is estimated on the PC from measured currents (P2, P5). |
| LDO U4 part number is not on the schematic | Its quiescent current is unknown and may dominate sleep current. Must be measured. |
| Flash, LoRa module and MCU are always powered from 3V3 | Sleep current depends on putting each part into its own sleep mode by command. |
| Only the sensor rail is switched (Q1, PB7 low = on) | Sensors cost nothing in sleep when PB7 is high. |
| PA0 (J1, digital sensor pin) is shorted to GND | Only J2 / PA1 is usable until this is repaired. |

Sleep current target, from datasheet typical values:

| Part | Sleep mode | Typical |
|---|---|---|
| STM32L432 | Stop 2 with RTC | 1.28 µA |
| W25Q80DV | Power-down (command 0xB9) | 1 µA (10 µA if left in standby) |
| SX1262 | Sleep, warm start (config kept) | 0.6 µA (module datasheet says 1.62 µA) |
| LDO U4, Q2, D1 | - | unknown, measure |

So about 3 to 4 µA plus the LDO, against about 15 mA measured with the current always-on test firmware.

---

## 3. Code folder layout

`../code/` follows the STM32CubeMX project layout, so it can be opened in STM32CubeIDE later or built with `make`.

```
code/
  Core/
    Inc/        main.h, board.h (all pin names), stm32l4xx_hal_conf.h, stm32l4xx_it.h
    Src/        main.c, stm32l4xx_it.c, stm32l4xx_hal_msp.c, system_stm32l4xx.c
    Startup/    startup_stm32l432xx.s
  Drivers/
    CMSIS/                    ST device headers
    STM32L4xx_HAL_Driver/     ST HAL
    BSP/                      one driver per chip on the board
      bsp_flash.c/.h          W25Q80DV
      bsp_radio.c/.h          SX1262 (wraps the Semtech driver)
      bsp_sensor.c/.h         sensor power switch, moisture ADC, DS18B20
  Middlewares/Third_Party/
    sx126x_driver/            Semtech driver
    tinyusb/                  USB CDC stack
  App/
    Inc/, Src/                product logic, no register access here
      app.c         mode state machine (normal / USB)
      cycle.c       one collection cycle
      datalog.c     records in flash
      config.c      settings in flash
      timekeep.c    RTC, wake-up timer, time sync
      link.c        LoRa packets and ACKs
      usb_cli.c     USB commands
      power.c       Stop 2 entry and exit
      timing.c      stage timers for measurements
  tools/            dfu_flash.py, swd_flash.sh (copied from ~/stm32l432_boardtest)
  web/              index.html, app.js (the PC web page)
  Makefile, STM32L432KCUx_FLASH.ld
```

Rules:
- `Core` holds only start-up, clock, interrupt handlers and `main()`.
- `Drivers/BSP` talks to hardware and knows nothing about the product.
- `App` holds the product behaviour and calls only BSP functions.
- `main()` stays short: init, then `App_Run()` in a loop.

---

## 4. Normal cycle (P1, P4, P5, P6)

Order follows page 1. The sensor is switched on first so its warm-up time overlaps the LoRa send.

```
RTC wake-up timer
  |
  1  Power_ExitStop()            restore clock
  2  Sensor_PowerOn()            start warm-up, note the start time
  3  Flash_Wake()                release from power-down
  4  Config_Load()               only on first boot; later kept in RAM
  5  if send enabled (P5):
        Radio_Wake()
        Link_SendBacklog()       unsent records, oldest first, not older than 3 days
        Radio_Sleep()
  6  Sensor_WaitWarmup()         wait only the time still missing since step 2
  7  Sensor_ReadAll()
  8  Sensor_PowerOff()
  9  Datalog_Append()            save the new record
 10  Flash_Sleep()
 11  Time_ArmNextWake()
 12  Power_EnterStop()
```

Points to note:
- A new reading is saved after the send, so it goes out on the next cycle, one period late. This is how page 1 is written. See decision D3.
- If a LoRa send fails, nothing is retried in this cycle. The record stays unsent and goes next time (P6).
- A cap on packets per cycle keeps on-time bounded when there is a large backlog.
- Each stage is timed by `timing.c` so the measurements on page 7 come for free.

---

## 5. Modules and functions

Each function is small and does one thing. Names follow the CubeMX style `Module_Action`.

### 5.1 Core

| Function | Job |
|---|---|
| `SystemClock_Config()` | Clock for normal mode |
| `SystemClock_ConfigUSB()` | HSI48 + CRS for USB mode |
| `MX_GPIO_Init()` | All pins to their safe idle state |
| `MX_SPI1_Init()` | SPI1 for flash and radio |
| `MX_ADC1_Init()` | ADC for PA1 and VREFINT |
| `MX_RTC_Init()` | RTC on LSI |
| `MX_IWDG_Init()` | Watchdog (phase 8) |

### 5.2 BSP flash (`bsp_flash`)

| Function | Job |
|---|---|
| `Flash_Init()` | Check JEDEC ID is EF 40 14 |
| `Flash_Wake()` | Command 0xAB, wait 3 µs |
| `Flash_Sleep()` | Command 0xB9 |
| `Flash_Read(addr, buf, len)` | Read |
| `Flash_Write(addr, buf, len)` | Page program with read-back check |
| `Flash_EraseSector(addr)` | 4 KB erase (45 ms typical, 300 ms max) |
| `Flash_WaitReady(timeout_ms)` | Poll the busy bit |

### 5.3 BSP radio (`bsp_radio`)

| Function | Job |
|---|---|
| `Radio_Init()` | Cold start: reset, TCXO on DIO3 at 1.8 V, DIO2 RF switch, DCDC, 866 MHz, SF7, BW 125 kHz |
| `Radio_Wake()` | Warm start from sleep (about 340 µs) |
| `Radio_Sleep()` | Sleep with configuration kept |
| `Radio_Send(buf, len, timeout_ms)` | Transmit, wait for TX done on DIO1 |
| `Radio_Receive(buf, max, window_ms, &rssi, &snr)` | Listen for the ACK, RF_SW high only here |
| `Radio_SetTxPower(dbm)` | From settings |

### 5.4 BSP sensors (`bsp_sensor`)

| Function | Job |
|---|---|
| `Sensor_PowerOn()` / `Sensor_PowerOff()` | PB7 low / high |
| `Sensor_WaitWarmup()` | Wait the remaining warm-up time |
| `Sensor_ReadMoisture_mV()` | PA1 in analog mode, average of N samples |
| `Sensor_ReadDS18B20()` | 1-Wire read with CRC check |
| `Sensor_ReadVdda_mV()` | From VREFINT |
| `Sensor_ReadAll(&reading)` | Fill one reading |
| `Sensor_SelfTest(&report)` | Read and say if values are in a sane range (P2) |

### 5.5 Data log (`datalog`)

| Function | Job |
|---|---|
| `Datalog_Init()` | At power-on, scan flash to find the write position and oldest unsent record |
| `Datalog_Append(&reading)` | Write one 16-byte record, erase the next sector when crossing into it |
| `Datalog_NextUnsent(&rec, max_age_s)` | Next record to send, skipping those older than 3 days |
| `Datalog_MarkSent(seq)` | Clear the "unsent" bit in the record |
| `Datalog_CountUnsent()` | For the "remaining" field in the LoRa packet (P6) |
| `Datalog_UsedPercent()` | For status (P2) |
| `Datalog_ReadRange(from, to, callback)` | For USB download (P3) |
| `Datalog_DeleteBefore(time)` | Delete by date (P3) |
| `Datalog_DeleteAll()` | Full delete (P3) |

Record, 16 bytes:

| Field | Size | Meaning |
|---|---|---|
| `seq` | 4 | Running number, never reused |
| `time` | 4 | Unix seconds |
| `moisture_mV` | 2 | PA1 |
| `temp_c100` | 2 | DS18B20 in 0.01 °C, 0x7FFF if not fitted |
| `vdda_mV` | 2 | Supply voltage |
| `flags` | 1 | bit0 unsent, bit1 not deleted, bit2 time valid, bit3 record type |
| `crc8` | 1 | Over all fields except `flags` |

Flash allows a bit to go from 1 to 0 without an erase. So "sent" and "deleted" are marked by clearing a bit inside the record itself. This is the "last sent data pointer" from page 6, and it survives power loss with no separate pointer to corrupt.

Flash map (256 sectors of 4 KB):

| Sectors | Use |
|---|---|
| 0, 1 | Settings, two copies |
| 2 to 255 | Data ring, 65,024 records |

Capacity: 45 days at one reading per minute, 451 days at one per 10 minutes. When the ring is full the oldest sector is erased.

### 5.6 Settings (`config`)

| Function | Job |
|---|---|
| `Config_Load()` | Read the newest valid copy, fall back to defaults |
| `Config_Save()` | Write the other copy with a CRC and a higher version number |
| `Config_Get()` / `Config_Set(key, value)` | Access with range checks |

Settings (P2, P5): collection period in seconds, send enabled (the tick box), records per LoRa packet (transmit length), packets per cycle, sensor warm-up in ms, TX power in dBm, device ID.

Flash only needs to be awake when settings are loaded at boot or saved from USB (P4). After that they stay in RAM, which Stop 2 keeps.

### 5.7 Time (`timekeep`)

| Function | Job |
|---|---|
| `Time_Init()` | Start RTC on LSI, detect if time was lost |
| `Time_Now()` | Unix seconds |
| `Time_Set(unix, source)` | Set from USB or LoRa, log a time-change record |
| `Time_IsValid()` | False after power loss until the next sync |
| `Time_CalibrateLSI()` | Measure LSI against a better clock and correct the RTC rate |
| `Time_ArmNextWake()` | Program the wake-up timer for the next period |

### 5.8 LoRa link (`link`)

| Function | Job |
|---|---|
| `Link_BuildPacket(buf, &len)` | Header plus up to N records |
| `Link_SendBacklog()` | Loop: build, send, wait ACK, mark sent; stop at the first failure or the packet cap |
| `Link_ParseAck(buf, len)` | Check device ID and sequence, pick up gateway time |

Data packet: device ID (2), packet type (1), record count (1), records still remaining (2) (P6), then the records.
ACK packet: device ID (2), highest sequence received (4), gateway Unix time (4).

### 5.9 USB (`usb_cli`)

The MCU side stays simple: text commands in, text lines out. The PC does the rest (P3).

| Command | Reply | From |
|---|---|---|
| `ID?` | device ID, firmware version, protocol version | - |
| `STATUS?` | time, time valid, flash used %, total and unsent records, last cycle result, VDDA | P2 |
| `SENSOR?` | live values and OK / FAIL per sensor | P2 |
| `TIME=<unix>` | sets the clock | P2 |
| `CFG?` | all settings | P2, P5 |
| `CFG <key>=<value>` | change one setting and save | P2, P5 |
| `DATA? <from> <to>` | one CSV line per record, then `END <count>` | P3 |
| `ERASE BEFORE <unix>` | delete older records | P3 |
| `ERASE ALL` | full delete | P3 |
| `TIMING?` | duration of each stage of the last cycle | P7 |
| `DFU` | jump to the ROM bootloader for firmware update | P5 |

### 5.10 Power and app

| Function | Job |
|---|---|
| `Power_EnterStop()` | Pins to idle, radio and flash asleep, enter Stop 2 |
| `Power_ExitStop()` | Restore the clock |
| `App_Run()` | Decide: run a cycle, serve USB, or sleep |
| `Cycle_Run()` | The 12 steps of section 4 |
| `Timing_Start(stage)` / `Timing_Stop(stage)` | Stage timers |

---

## 6. PC web page (P2, P3, P5)

One HTML file and one JS file, using the browser's Web Serial API (Chrome or Edge). No install.

| Screen | Does |
|---|---|
| Status | Shows `STATUS?` and `SENSOR?` |
| Settings | Set time from the PC clock, period, send tick box, transmit length |
| Run-time | From the period and a table of measured currents, shows how long a full charge lasts |
| Data | Download a date range, plot against time, export CSV |
| Delete | Before a date, or everything |
| Firmware | Sends `DFU`, then loads a `.bin` over WebUSB DFU |

The run-time figure is computed on the PC: charge per cycle (sum of current × time for each stage) divided by the period, plus sleep current, against battery capacity. The currents come from phase 7.

---

## 7. Time procedure (P6)

There is no crystal, so time needs three layers:

1. **Calibrate LSI.** Measure LSI against HSI16 with a timer capture and correct the RTC rate. This brings the error from about ±3 % to about ±1 %.
2. **Sync over USB.** The web page sends the PC time. While USB is connected, HSI48 is locked to the USB bus, which gives a much better reference for calibrating LSI.
3. **Sync over LoRa.** Every ACK carries the gateway time. If the clock is off by more than 2 seconds, correct it.

Rules for the stored timestamps:
- Each time the clock is stepped, a time-change record (old time, new time) is logged, so the PC can correct earlier records.
- After a power loss, records are flagged "time not valid" until the next sync.

The ESP32 gateway therefore needs a real clock, from Wi-Fi NTP or the PC.

---

## 8. USB detection

There is no VBUS pin on the MCU. Proposed method, to be proven in phase 5:

- In sleep, PA12 (D+) is an input with the internal pull-up and a falling-edge interrupt.
- Unplugged, the line floats high. Plugged into a PC, the host's 15 kΩ pull-down brings it low and wakes the MCU.
- The MCU then starts HSI48 and the USB stack. On USB suspend or disconnect it goes back to normal mode.

Fallback if this is unreliable: pressing RESET with the cable in opens a 5-second USB window at boot.

---

## 9. Work phases

Each phase ends with a test that must pass before the next one starts.

| # | Phase | Work | Exit test |
|---|---|---|---|
| 0 | Project skeleton | Create the `code/` tree, Makefile, HAL config; port clock, GPIO, SPI, ADC, USB CDC from `~/stm32l432_boardtest` | Builds; `ID?` answers over USB; flash ID and radio status read |
| 1 | BSP drivers | `bsp_flash`, `bsp_radio`, `bsp_sensor`, each with sleep and wake | Self-test command passes for each; flash and radio sleep currents measured |
| 2 | Time and low power | RTC on LSI, wake-up timer, Stop 2, LSI calibration | Wakes on schedule for 1 hour; sleep current and clock drift recorded |
| 3 | Data log and settings | Records, boot scan, sent and deleted bits, ring wrap, two-copy settings | Power cut during write loses at most one record; wrap test passes with a shrunk ring |
| 4 | Normal cycle and LoRa link | `Cycle_Run()`, packets, ACK with time, ESP32 gateway update | 24-hour run: gateway record count equals flash record count |
| 5 | USB mode | Plug detection, all commands, DFU jump | Every command works from a serial terminal; plug and unplug 20 times |
| 6 | Web page | The six screens of section 6 | Download, plot, delete, settings and firmware update all work from the browser |
| 7 | Timing and power (P7) | Fill the tables below, save in `../results/` | Run-time estimate agrees with a measured multi-day run |
| 8 | Hardening | Watchdog, brown-out handling, failure paths | 7-day run with no lost records and no hang |

Phase 7 tables (P7):

| Timing | Power |
|---|---|
| LoRa TX time per packet size | Sensor only |
| ACK wait and time-out | Flash only |
| Flash read, write, erase times | LoRa only |
| Sensor warm-up and read time | Flash + LoRa |
| Full cycle time | Flash + sensors |
| | Flash + LoRa + sensors |
| | Total active current |
| | Total sleep current |

---

## 10. Decisions to confirm

The plan uses the first option of each unless you say otherwise.

| # | Question | Plan uses | Alternative |
|---|---|---|---|
| D1 | Libraries | ST HAL for peripherals, TinyUSB for USB (already proven on this board) | ST's own USB device library, which is the CubeMX default but untested here |
| D2 | Clock in normal mode | HSI16, with HSI48 only in USB mode. Lower current and no start-up wait | 48 MHz always, as on page 1 |
| D3 | Order in a cycle | Send old data first, then read, as on page 1. New reading goes out one period late | Read first, then send, so the reading goes out in the same cycle but on-time is longer |
| D4 | Sensors in version 1 | Moisture on J2 only | Add DS18B20 on J1 after the PA0 short is repaired |
| D5 | Gateway | The XIAO ESP32S3 kit, extended to send ACKs with time | A different gateway |

## 11. Open hardware items

- PA0 (J1) shorted to GND: not located yet.
- USB connector contact is intermittent: this will disturb phases 5 and 6.
- LDO U4 part number and quiescent current: needed for the sleep budget.
- No battery-voltage sense and no LED: worth adding on the next board revision, along with a 32.768 kHz crystal and a VBUS sense line.
