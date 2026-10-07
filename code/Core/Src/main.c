/*
 * STM32L432 LoRa sensor logger: start-up, clock and peripheral init.
 * The product behaviour is in App/.
 */
#include "main.h"
#include "app.h"
#include "app_hooks.h"
#include "powertest.h"

SPI_HandleTypeDef hspi1;
ADC_HandleTypeDef hadc1;
RTC_HandleTypeDef hrtc;
IWDG_HandleTypeDef hiwdg;

/* survives a software reset (not cleared by the start-up code) */
uint32_t g_boot_request __attribute__((section(".noinit")));
uint32_t g_reset_flags;

static void Boot_CheckDfuRequest(void);
static void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_SPI1_Init(void);
static void MX_RTC_Init(void);
static void MX_IWDG_Init(void);

int main(void) {
  Boot_CheckDfuRequest();

  HAL_Init();
  SystemClock_Config();
  g_reset_flags = RCC->CSR;
  __HAL_RCC_CLEAR_RESET_FLAGS();

  MX_GPIO_Init();
  MX_SPI1_Init();
  MX_RTC_Init();
  MX_IWDG_Init();
  Board_UsInit();

#ifdef POWER_TEST
  PowerTest_Run();
#else
  App_Init();
  while (1) {
    App_Run();
  }
#endif
}

/* A firmware update is started by setting g_boot_request and resetting. The
 * jump to the ROM bootloader is made here, before any peripheral or the
 * watchdog has been started. */
static void Boot_CheckDfuRequest(void) {
  if (g_boot_request != BOOT_REQUEST_DFU) return;
  g_boot_request = 0;
  RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
  (void) RCC->APB2ENR;
  SYSCFG->MEMRMP = 1; /* system memory at address 0 */
  const uint32_t *sys = (const uint32_t *) 0x1FFF0000UL;
  __set_MSP(sys[0]);
  ((void (*)(void)) sys[1])();
  while (1) {}
}

/* SYSCLK = HSI16 (the board has no crystal), LSI for the RTC and watchdog.
 * HSI16 is also the clock the MCU wakes up on after Stop 2. */
static void SystemClock_Config(void) {
  RCC_OscInitTypeDef osc = {0};
  RCC_ClkInitTypeDef clk = {0};

  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);

  osc.OscillatorType = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_LSI;
  osc.HSIState = RCC_HSI_ON;
  osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  osc.LSIState = RCC_LSI_ON;
  osc.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK) Error_Handler();

  clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clk.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clk.APB1CLKDivider = RCC_HCLK_DIV1;
  clk.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_0) != HAL_OK) Error_Handler();

  __HAL_RCC_WAKEUPSTOP_CLK_CONFIG(RCC_STOP_WAKEUPCLOCK_HSI);
  /* MSI was the reset clock and is not needed any more */
  __HAL_RCC_MSI_DISABLE();
}

/* Every pin to its safe idle state. Unused pins are analog (lowest current).
 * PA13/PA14 stay as SWD. */
static void MX_GPIO_Init(void) {
  GPIO_InitTypeDef g = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  g.Mode = GPIO_MODE_ANALOG;
  g.Pull = GPIO_NOPULL;
  g.Pin = GPIO_PIN_All & ~(GPIO_PIN_13 | GPIO_PIN_14);
  HAL_GPIO_Init(GPIOA, &g);
  g.Pin = GPIO_PIN_All;
  HAL_GPIO_Init(GPIOB, &g);
  g.Pin = GPIO_PIN_14 | GPIO_PIN_15;
  HAL_GPIO_Init(GPIOC, &g);
  __HAL_RCC_GPIOC_CLK_DISABLE();

  /* chip selects high before the SPI bus is used */
  HAL_GPIO_WritePin(FLASH_CS_PORT, FLASH_CS_PIN, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LORA_NSS_PORT, LORA_NSS_PIN, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LORA_RST_PORT, LORA_RST_PIN, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LORA_RFSW_PORT, LORA_RFSW_PIN, GPIO_PIN_RESET);

  g.Mode = GPIO_MODE_OUTPUT_PP;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  g.Pin = FLASH_CS_PIN;
  HAL_GPIO_Init(FLASH_CS_PORT, &g);
  g.Pin = LORA_NSS_PIN;
  HAL_GPIO_Init(LORA_NSS_PORT, &g);
  g.Pin = LORA_RST_PIN;
  HAL_GPIO_Init(LORA_RST_PORT, &g);
  g.Pin = LORA_RFSW_PIN;
  HAL_GPIO_Init(LORA_RFSW_PORT, &g);

  g.Mode = GPIO_MODE_INPUT;
  g.Pin = LORA_BUSY_PIN;
  HAL_GPIO_Init(LORA_BUSY_PORT, &g);
  g.Pull = GPIO_PULLDOWN;
  g.Pin = LORA_DIO1_PIN;
  HAL_GPIO_Init(LORA_DIO1_PORT, &g);

  /* SENSOR_EN as input: R1 keeps Q1 off, so the sensor rail is off */
  g.Pull = GPIO_NOPULL;
  g.Pin = SENSOR_EN_PIN;
  HAL_GPIO_Init(SENSOR_EN_PORT, &g);
}

/* SPI1 mode 0, 8-bit, 16 MHz / 8 = 2 MHz (the speed proven in bring-up) */
static void MX_SPI1_Init(void) {
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 7;
  if (HAL_SPI_Init(&hspi1) != HAL_OK) Error_Handler();
}

/* ADC1, single conversions started by software. Called before each sensor
 * read; the ADC is de-initialised (deep power-down) afterwards. */
void MX_ADC1_Init(void) {
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV1;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  hadc1.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc1) != HAL_OK) Error_Handler();
  if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK) Error_Handler();
}

/* RTC on LSI. The calendar keeps running through resets; timekeep.c decides
 * whether it has to be started from zero. */
static void MX_RTC_Init(void) {
  RCC_PeriphCLKInitTypeDef pclk = {0};

  HAL_PWR_EnableBkUpAccess();
  pclk.PeriphClockSelection = RCC_PERIPHCLK_RTC;
  pclk.RTCClockSelection = RCC_RTCCLKSOURCE_LSI;
  if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK) Error_Handler();

  hrtc.Instance = RTC;
  hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
  hrtc.Init.AsynchPrediv = 127;
  hrtc.Init.SynchPrediv = 255;
  hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
  hrtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;
  hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
  hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
  if (HAL_RTC_Init(&hrtc) != HAL_OK) Error_Handler();
}

/* Independent watchdog, about 32 s. It keeps running in Stop 2, so the
 * firmware never sleeps longer than a few seconds at a time. */
static void MX_IWDG_Init(void) {
  hiwdg.Instance = IWDG;
  hiwdg.Init.Prescaler = IWDG_PRESCALER_256;
  hiwdg.Init.Reload = 4095;
  hiwdg.Init.Window = IWDG_WINDOW_DISABLE;
  if (HAL_IWDG_Init(&hiwdg) != HAL_OK) Error_Handler();
}

void Board_UsInit(void) {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* wraps after about 268 s at 16 MHz: use only for short intervals */
uint32_t Board_Micros(void) { return DWT->CYCCNT / (SystemCoreClock / 1000000u); }

void Board_DelayUs(uint32_t us) {
  uint32_t t0 = DWT->CYCCNT;
  uint32_t n = us * (SystemCoreClock / 1000000u);
  while (DWT->CYCCNT - t0 < n) {}
}

void Board_DelayMs(uint32_t ms) {
  uint32_t t0 = HAL_GetTick();
  while (HAL_GetTick() - t0 < ms) Board_Idle();
}

/* Unrecoverable init error: wait for the watchdog to reset the board. */
void Error_Handler(void) {
  __disable_irq();
  while (1) {}
}
