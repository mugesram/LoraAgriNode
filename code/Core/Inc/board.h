/*
 * Board definitions for entc-sensornode-1, the custom STM32L432KCU6 LoRa
 * sensor logger.
 * Pin map from the schematic (MCU_Stm.SchDoc, 2026-09-21).
 */
#ifndef BOARD_H
#define BOARD_H

#include "stm32l4xx_hal.h"

/* SPI1, shared by the flash and the radio */
#define SPI1_SCK_PIN        GPIO_PIN_5   /* PA5, R7 100k to GND */
#define SPI1_MISO_PIN       GPIO_PIN_6   /* PA6 */
#define SPI1_MOSI_PIN       GPIO_PIN_7   /* PA7 */
#define SPI1_PORT           GPIOA

/* W25Q80DV */
#define FLASH_CS_PIN        GPIO_PIN_0   /* PB0, R4 10k pull-up */
#define FLASH_CS_PORT       GPIOB

/* Wio-SX1262 */
#define LORA_NSS_PIN        GPIO_PIN_4   /* PA4, no pull-up on the board */
#define LORA_NSS_PORT       GPIOA
#define LORA_DIO1_PIN       GPIO_PIN_8   /* PA8 */
#define LORA_DIO1_PORT      GPIOA
#define LORA_BUSY_PIN       GPIO_PIN_9   /* PA9 */
#define LORA_BUSY_PORT      GPIOA
#define LORA_RST_PIN        GPIO_PIN_10  /* PA10 */
#define LORA_RST_PORT       GPIOA
#define LORA_RFSW_PIN       GPIO_PIN_5   /* PB5 = RXEN, high only while receiving */
#define LORA_RFSW_PORT      GPIOB

/* Sensors */
#define SENSOR_EN_PIN       GPIO_PIN_7   /* PB7, low = 3V3_SENSOR on, R1 10k pull-up */
#define SENSOR_EN_PORT      GPIOB
#define SENSOR_ANA_PIN      GPIO_PIN_1   /* PA1, J2, ADC1_IN6 */
#define SENSOR_ANA_PORT     GPIOA
#define SENSOR_ANA_CHANNEL  ADC_CHANNEL_6
/* 1-Wire data line. J1 (PA0) is the digital connector, but PA0 is shorted to
 * GND on the first board, so the DS18B20 goes on J2 (PA1) for now. Change to
 * 0 when PA0 is repaired. */
#define SENSOR_OW_PIN_NUM   1
#define SENSOR_OW_PORT      GPIOA

/* USB */
#define USB_DM_PIN          GPIO_PIN_11  /* PA11 */
#define USB_DP_PIN          GPIO_PIN_12  /* PA12 */
#define USB_PORT            GPIOA

extern SPI_HandleTypeDef hspi1;
extern ADC_HandleTypeDef hadc1;
extern RTC_HandleTypeDef hrtc;
extern IWDG_HandleTypeDef hiwdg;

void MX_ADC1_Init(void);

/* microsecond timing from the DWT cycle counter */
void     Board_UsInit(void);
uint32_t Board_Micros(void);
void     Board_DelayUs(uint32_t us);
/* millisecond delay that keeps the watchdog and USB serviced */
void     Board_DelayMs(uint32_t ms);

#endif
