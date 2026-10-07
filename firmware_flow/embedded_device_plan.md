# Embedded Device Plan (Handwritten Notes, Full Transcription)

Transcribed from handwritten notebook pages. Original wording and spelling kept; corrections or unclear words are in [brackets].

---

## Page 1: Without USB interrupt

without usb interrupt
=> normal workflow.

=> It activate on start then by timer interupt

---

here all runs with full power serving

start mcu 48mhz
enable lora
enable flash, enable sensors *(small note squeezed in: "wait flash enable")*
read untransfered data from last to 3 days if there send
is lora enabled send this data
disable lora
~~read senso~~
ensure from sensor enable to now sensor enabling time passed
then read values
then disable sensors
save in flash

---

## Page 2: Status and setting

*(a scribbled-out word at the top)*

**Status**
=> sensor
  - read sensor values and print
    (command mcu, mcu send values, PC prints)
    also tell sensor works fine or not
- how much [%] of flash is full
- setted date time

**setting**
update time
~~Set collection type~~
~~by time~~
Set collection foreqeuncy
(we will give use data of power) so it tell in this frequency how long run in full charge

---

## Page 3: USB interrupt

usb interrupt
↓
comes to usb mode
↓
way for commuination between pc and mcu
(simple web [code] code in JS + interface)
(no complex in mcu side just give data) (PC do rest)

- data
  - read flash data
  - empty / delete data by timestamp (date)
    (before this delete)
    (full delete)

=> dat wisualization
  => select data
  => visualize by ~~(crossed out word)~~ time axis

---

## Page 4: Flash sleep and parameters

~~update~~
~~(proper red~~
disable (sleep) flash
(keep proper read & write functions here for flash)

have to have the parameters set by web stored in flash so flash might have to activated first (but no need all time)
then sleep 2/ [unclear mark]
activates on timer interupt by the collection fereqency.

---

## Page 5: Send data, LoRa, firmware update

- is send data
  => (check box)
  if ticked will send data after collection or just only saves to flash

lora transmite length set

=> after all it will give how long it can run in full charge.

=> firmware update
  => it takes relavant then suitable software can update.
  => if they load the firmware can upload.

---

## Page 6: Failure handling, tracking, code structure

if fails just leave we can send in next turn.

also in last message send to server send how many collection remaining send do [done?]

(last send data pointer tracked so above that have to send)

All to minimise on time and reduce on time power
Proper procedure for timestamp update

maintain proper stm32 structure for code base

---

## Page 7: Timing and power measurements

finding time out
finding transmission both side timming
read, write times
sensor reading part timing
full timing details

**Power**
only sensor on
only flash
only lora
flash + lora
flash + sensors
flash + lora + sensor

total active work current
total ~~(crossed-out word)~~ sleep current

---

## Uncertain words

- Page 2: the "%" in "how much % of flash is full" looks like a small inserted mark.
- Page 4: "then sleep 2/" has an unclear symbol after "sleep".
- Page 6: "send do" may be "send done".
