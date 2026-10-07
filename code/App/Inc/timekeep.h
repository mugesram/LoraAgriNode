/*
 * Time. The board has no 32.768 kHz crystal, so the RTC runs on the LSI RC
 * oscillator (about +-3 %). The RTC calendar is used only as a raw seconds
 * counter. Real time is
 *     unix = ref_unix + (raw - ref_raw) * rate
 * where rate is first taken from a measurement of LSI against HSI16 and then
 * corrected every time the clock is set (USB or LoRa ACK). The state is kept
 * in RTC backup registers, so it survives resets and firmware updates as
 * long as the board stays powered.
 */
#ifndef TIMEKEEP_H
#define TIMEKEEP_H

#include <stdbool.h>
#include <stdint.h>

void     Time_Init(void);
uint32_t Time_Now(void);            /* Unix seconds; seconds since power-up if not valid */
bool     Time_IsValid(void);
uint32_t Time_Set(uint32_t unix_s); /* returns the time before the change */
uint32_t Time_Raw(void);            /* RTC seconds since power-up, uncorrected */
uint32_t Time_LsiHz(void);          /* LSI frequency measured at power-up, 0 = failed */
int32_t  Time_RatePpm(void);        /* how far the RTC second is from a real second */
uint32_t Time_MeasureLsi(void);

void     Time_ArmWake(uint32_t seconds);  /* RTC wake-up timer, 1 s steps */
bool     Time_WakeFired(void);            /* true once after each wake-up interrupt */

#endif
