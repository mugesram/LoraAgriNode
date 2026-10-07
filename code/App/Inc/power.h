/* Stop 2 sleep. */
#ifndef POWER_H
#define POWER_H

/* Puts the flash, radio and sensors to sleep and enters Stop 2. Returns
 * after the next interrupt (the RTC wake-up timer). */
void Power_EnterStop(void);

#endif
