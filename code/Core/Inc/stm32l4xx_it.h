#ifndef STM32L4xx_IT_H
#define STM32L4xx_IT_H

void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void SysTick_Handler(void);
void RTC_WKUP_IRQHandler(void);
void USB_IRQHandler(void);

#endif
