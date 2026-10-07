/* Top level: decides between a collection cycle, USB service and sleep. */
#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  bool flash_ok;      /* W25Q80 answered with the right ID */
  bool config_stored; /* settings came from the flash (false = defaults) */
  bool log_ok;        /* data log scan worked */
  bool radio_ok;      /* SX1262 initialised */
  bool formatted;     /* the flash held no settings: it was erased at this start */
} app_health_t;

extern app_health_t g_health;

void App_Init(void);
void App_Run(void);
bool App_UsbService(void);        /* starts, serves and stops USB; true while USB is on */

/* Sets the clock. Logs a time-change record and moves the next collection
 * time by the same step. */
void App_SetTime(uint32_t unix_s);
void App_Reschedule(void);        /* after the period was changed */
void App_RequestCycle(void);      /* run one cycle now */
uint32_t App_SecondsToNextCycle(void);
void App_RebootToDfu(void);
void App_Reboot(void);

#endif
