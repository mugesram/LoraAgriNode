/* MCU support package: clocks, pins and interrupts of each peripheral. */
#include "main.h"

void HAL_MspInit(void) {
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  __HAL_RCC_PWR_CLK_ENABLE();
}

void HAL_SPI_MspInit(SPI_HandleTypeDef *hspi) {
  GPIO_InitTypeDef g = {0};
  if (hspi->Instance != SPI1) return;
  __HAL_RCC_SPI1_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  g.Mode = GPIO_MODE_AF_PP;
  g.Speed = GPIO_SPEED_FREQ_HIGH;
  g.Alternate = GPIO_AF5_SPI1;
  g.Pull = GPIO_NOPULL;
  g.Pin = SPI1_SCK_PIN | SPI1_MOSI_PIN;
  HAL_GPIO_Init(SPI1_PORT, &g);
  /* MISO floats when both chips are deselected: hold it with a pull-up */
  g.Pull = GPIO_PULLUP;
  g.Pin = SPI1_MISO_PIN;
  HAL_GPIO_Init(SPI1_PORT, &g);
}

void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc) {
  if (hadc->Instance != ADC1) return;
  __HAL_RCC_ADC_CLK_ENABLE();
}

void HAL_ADC_MspDeInit(ADC_HandleTypeDef *hadc) {
  if (hadc->Instance != ADC1) return;
  __HAL_RCC_ADC_CLK_DISABLE();
}

void HAL_RTC_MspInit(RTC_HandleTypeDef *h) {
  if (h->Instance != RTC) return;
  __HAL_RCC_RTCAPB_CLK_ENABLE();
  __HAL_RCC_RTC_ENABLE();
  HAL_NVIC_SetPriority(RTC_WKUP_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(RTC_WKUP_IRQn);
}

void HAL_TIM_IC_MspInit(TIM_HandleTypeDef *htim) {
  if (htim->Instance == TIM16) __HAL_RCC_TIM16_CLK_ENABLE();
}

void HAL_TIM_IC_MspDeInit(TIM_HandleTypeDef *htim) {
  if (htim->Instance == TIM16) __HAL_RCC_TIM16_CLK_DISABLE();
}
