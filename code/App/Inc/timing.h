/* Duration of each stage of the last collection cycle, for the power budget. */
#ifndef TIMING_H
#define TIMING_H

#include <stdint.h>

typedef enum {
  TM_FLASH_WAKE,    /* flash out of power-down */
  TM_RADIO_WAKE,    /* radio out of sleep */
  TM_LORA_SEND,     /* whole backlog send, all packets and ACK waits */
  TM_LORA_TX,       /* time on air, all packets of the cycle */
  TM_LORA_ACK,      /* time listening for replies, all packets of the cycle */
  TM_SENSOR_WAIT,   /* extra wait for the sensor warm-up */
  TM_SENSOR_READ,
  TM_FLASH_SAVE,    /* record write with read-back (and sector erase if due) */
  TM_CYCLE,         /* whole cycle, wake-up to ready-for-sleep */
  TM_COUNT
} timing_stage_t;

void        Timing_Start(timing_stage_t s);
void        Timing_Stop(timing_stage_t s);
void        Timing_Add(timing_stage_t s, uint32_t us);
void        Timing_Clear(void);
uint32_t    Timing_Us(timing_stage_t s);
const char *Timing_Name(timing_stage_t s);

#endif
