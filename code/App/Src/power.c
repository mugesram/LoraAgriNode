/* Low-power sleep. */
#include "board.h"
#include "bsp_flash.h"
#include "bsp_radio.h"
#include "bsp_sensor.h"
#include "power.h"

void Power_EnterStop(void) {
  Sensor_PowerOff();
  Flash_Sleep();
  Radio_Sleep();

  /* debug trace clock off while asleep */
  CoreDebug->DEMCR &= ~CoreDebug_DEMCR_TRCENA_Msk;
  HAL_SuspendTick();
  HAL_PWREx_EnterSTOP2Mode(PWR_STOPENTRY_WFI);
  /* awake again on HSI16, which is also the system clock in run mode */
  HAL_ResumeTick();
  Board_UsInit();
}
