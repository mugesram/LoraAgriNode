/*
 * Sensors: switched 3V3_SENSOR rail, analog moisture sensor on J2 (PA1),
 * DS18B20 on the 1-Wire pin, and the supply voltage from VREFINT.
 */
#ifndef BSP_SENSOR_H
#define BSP_SENSOR_H

#include <stdbool.h>
#include <stdint.h>

#define SENSOR_MODE_MOISTURE  0   /* analog sensor on J2 */
#define SENSOR_MODE_DS18B20   1   /* DS18B20 on the 1-Wire pin */

#define SENSOR_NO_MOISTURE    0xFFFFu
#define SENSOR_NO_TEMP        0x7FFF

typedef struct {
  uint16_t moisture_mV;   /* SENSOR_NO_MOISTURE if not read */
  int16_t  temp_c100;     /* 0.01 degC, SENSOR_NO_TEMP if not read */
  uint16_t vdda_mV;
  bool     ok;            /* the selected sensor answered with a sane value */
} sensor_reading_t;

void Sensor_PowerOn(void);
void Sensor_PowerOff(void);
bool Sensor_IsOn(void);
void Sensor_WaitWarmup(uint32_t warmup_ms);       /* waits only the time still missing */
bool Sensor_ReadAll(uint8_t mode, sensor_reading_t *out);

#endif
