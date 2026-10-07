/*
 * Power-supply test builds (make TEST=lora|flash|sensor|cycle). Each build
 * runs one part of the board over and over from power-up, so that its
 * current can be measured, and records its own timing.
 *
 *   lora    radio only: ping + one data packet every period, flash and
 *           sensor off. The timing of each round travels in the next packet.
 *   flash   external flash only: 10 s reading, 10 s erasing and writing,
 *           10 s power-down, repeated. Timing goes to the timing log.
 *   sensor  sensor rail only: three settling measurements, then the sensor
 *           stays on. Timing goes to the timing log.
 *   cycle   the real collection cycle every period. Timing goes to the
 *           timing log.
 *
 * The test runs only while no PC is on the USB cable. On USB the normal
 * commands work, plus TLOG? (timing log), TCLEAR (erase it) and TRUN (run
 * a round now, for a bench check).
 */
#ifndef POWERTEST_H
#define POWERTEST_H

#define PT_LORA    1
#define PT_FLASH   2
#define PT_SENSOR  3
#define PT_CYCLE   4

void PowerTest_Run(void);    /* never returns */
void PowerTest_Round(void);  /* one round (three for lora and cycle) without sleeping, for TRUN */

#endif
