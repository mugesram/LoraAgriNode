/* Interrupt handlers. */
#include "main.h"
#include "stm32l4xx_it.h"
#include "tusb.h"

void NMI_Handler(void) { while (1) {} }
/* faults: stop here, the watchdog resets the board */
void HardFault_Handler(void) { while (1) {} }
void MemManage_Handler(void) { while (1) {} }
void BusFault_Handler(void) { while (1) {} }
void UsageFault_Handler(void) { while (1) {} }

void SysTick_Handler(void) { HAL_IncTick(); }

void RTC_WKUP_IRQHandler(void) { HAL_RTCEx_WakeUpTimerIRQHandler(&hrtc); }

void USB_IRQHandler(void) { tud_int_handler(0); }
