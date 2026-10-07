/* One collection cycle: send old data, read the sensor, save, sleep. */
#ifndef CYCLE_H
#define CYCLE_H

#include <stdbool.h>
#include <stdint.h>
#include "bsp_sensor.h"

typedef struct {
  uint32_t count;          /* cycles run since reset */
  uint32_t time;           /* time of the last cycle */
  int      sent;           /* records acknowledged in the last cycle */
  bool     saved;          /* the new record was written to the flash */
  sensor_reading_t reading;
} cycle_result_t;

extern cycle_result_t g_cycle;

void Cycle_Run(void);

#endif
