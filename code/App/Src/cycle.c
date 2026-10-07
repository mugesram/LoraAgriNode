/*
 * The normal work cycle (handwritten plan, page 1). Everything is kept as
 * short as possible: the sensor warms up while the radio is busy.
 */
#include "bsp_flash.h"
#include "bsp_radio.h"
#include "bsp_sensor.h"
#include "config.h"
#include "cycle.h"
#include "datalog.h"
#include "link.h"
#include "timekeep.h"
#include "timing.h"

cycle_result_t g_cycle;

void Cycle_Run(void) {
  Timing_Clear();
  Timing_Start(TM_CYCLE);

  /* 1. sensor power first: its warm-up time overlaps the radio work */
  Sensor_PowerOn();

  /* 2. flash out of power-down */
  Timing_Start(TM_FLASH_WAKE);
  Flash_Wake();
  Timing_Stop(TM_FLASH_WAKE);

  /* 3. send the records that have not gone out yet, then radio off */
  g_cycle.sent = 0;
  if (g_cfg.send_enable && Datalog_CountUnsent() > 0) {
    Timing_Start(TM_RADIO_WAKE);
    bool radio_ok = Radio_Wake();
    Timing_Stop(TM_RADIO_WAKE);
    if (radio_ok) {
      Timing_Start(TM_LORA_SEND);
      g_cycle.sent = Link_SendBacklog();
      Timing_Stop(TM_LORA_SEND);
    }
  }
  Radio_Sleep();

  /* 4. wait only for what is still missing of the warm-up time, then read */
  Timing_Start(TM_SENSOR_WAIT);
  Sensor_WaitWarmup(g_cfg.warmup_ms);
  Timing_Stop(TM_SENSOR_WAIT);
  Timing_Start(TM_SENSOR_READ);
  Sensor_ReadAll(g_cfg.sensor_mode, &g_cycle.reading);
  Timing_Stop(TM_SENSOR_READ);
  Sensor_PowerOff();

  /* 5. save the new record; it is sent in the next cycle */
  g_cycle.time = Time_Now();
  Timing_Start(TM_FLASH_SAVE);
  g_cycle.saved = Datalog_Append(g_cycle.time, Time_IsValid(), g_cycle.reading.moisture_mV,
                                 g_cycle.reading.temp_c100, g_cycle.reading.vdda_mV);
  Timing_Stop(TM_FLASH_SAVE);

  /* 6. flash back to power-down */
  Flash_Sleep();

  g_cycle.count++;
  Timing_Stop(TM_CYCLE);
}
