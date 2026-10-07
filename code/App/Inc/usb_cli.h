/*
 * USB commands. One text line in, text lines out, ended by "OK" or
 * "ERR <reason>". The PC does all the presentation work.
 *
 *   ID?                     device id, firmware and protocol version
 *   STATUS?                 time, flash use, counters, last cycle
 *   SENSOR?                 powers the sensor, reads it, says if it works
 *   TIME=<unix>             set the clock
 *   CFG?                    all settings as "key=value min max"
 *   CFG <key>=<value>       change one setting and save it
 *   DEFAULTS                all settings back to default
 *   DATA? [from] [to]       stored records, one line each, then "END <n>"
 *   ERASE BEFORE <unix>     delete records older than the time
 *   ERASE ALL               delete everything
 *   TIMING?                 stage durations of the last cycle
 *   CYCLE                   run one collection cycle now
 *   DFU                     restart in the USB bootloader (firmware update)
 *   REBOOT
 */
#ifndef USB_CLI_H
#define USB_CLI_H

void UsbCli_Reset(void);
void UsbCli_Task(void);

#endif
