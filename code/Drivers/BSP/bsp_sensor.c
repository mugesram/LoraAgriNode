/* Sensor power switch, ADC readings and DS18B20. */
#include "board.h"
#include "app_hooks.h"
#include "bsp_sensor.h"

#define ADC_SAMPLES        16
#define MOISTURE_MIN_MV    100    /* below this the sensor gives no output */
#define DS18B20_CONVERT_MS 760    /* 12-bit conversion, 750 ms max */

static bool s_on;
static uint32_t s_on_tick;

/* ------------------------------------------------------------ power switch */
void Sensor_PowerOn(void) {
  GPIO_InitTypeDef g = {0};
  if (s_on) return;
  HAL_GPIO_WritePin(SENSOR_EN_PORT, SENSOR_EN_PIN, GPIO_PIN_RESET); /* low = Q1 on */
  g.Pin = SENSOR_EN_PIN;
  g.Mode = GPIO_MODE_OUTPUT_PP;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(SENSOR_EN_PORT, &g);
  s_on = true;
  s_on_tick = HAL_GetTick();
}

void Sensor_PowerOff(void) {
  GPIO_InitTypeDef g = {0};
  /* input: R1 pulls the gate high, no current through R1 */
  g.Pin = SENSOR_EN_PIN;
  g.Mode = GPIO_MODE_INPUT;
  g.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(SENSOR_EN_PORT, &g);
  /* sensor pin analog so that nothing feeds the unpowered sensor */
  g.Pin = SENSOR_ANA_PIN;
  g.Mode = GPIO_MODE_ANALOG;
  HAL_GPIO_Init(SENSOR_ANA_PORT, &g);
  s_on = false;
}

bool Sensor_IsOn(void) { return s_on; }

void Sensor_WaitWarmup(uint32_t warmup_ms) {
  while (HAL_GetTick() - s_on_tick < warmup_ms) Board_Idle();
}

/* --------------------------------------------------------------------- ADC */
static uint32_t adc_read(uint32_t channel) {
  ADC_ChannelConfTypeDef ch = {0};
  uint32_t sum = 0;
  int n = 0;

  ch.Channel = channel;
  ch.Rank = ADC_REGULAR_RANK_1;
  ch.SamplingTime = ADC_SAMPLETIME_640CYCLES_5;
  ch.SingleDiff = ADC_SINGLE_ENDED;
  ch.OffsetNumber = ADC_OFFSET_NONE;
  if (HAL_ADC_ConfigChannel(&hadc1, &ch) != HAL_OK) return 0;
  /* first conversion after a channel change is thrown away */
  for (int i = 0; i < ADC_SAMPLES + 1; i++) {
    if (HAL_ADC_Start(&hadc1) != HAL_OK) break;
    if (HAL_ADC_PollForConversion(&hadc1, 10) != HAL_OK) break;
    uint32_t v = HAL_ADC_GetValue(&hadc1);
    if (i > 0) { sum += v; n++; }
  }
  HAL_ADC_Stop(&hadc1);
  return n ? sum / (uint32_t) n : 0;
}

static uint16_t read_vdda_mV(void) {
  uint32_t raw = adc_read(ADC_CHANNEL_VREFINT);
  if (raw == 0) return 0;
  return (uint16_t) __HAL_ADC_CALC_VREFANALOG_VOLTAGE(raw, ADC_RESOLUTION_12B);
}

static uint16_t read_moisture_mV(uint16_t vdda_mV) {
  GPIO_InitTypeDef g = {0};
  /* the ADC only sees the pad in analog mode */
  g.Pin = SENSOR_ANA_PIN;
  g.Mode = GPIO_MODE_ANALOG;
  g.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(SENSOR_ANA_PORT, &g);
  uint32_t raw = adc_read(SENSOR_ANA_CHANNEL);
  return (uint16_t) (raw * vdda_mV / 4095u);
}

/* ------------------------------------------------------------------ 1-Wire */
/* The board has no 4.7k pull-up on the data line: the internal pull-up
 * releases the bus and the pin is driven high whenever the sensor is not
 * allowed to talk. Slot timing comes from the DWT cycle counter, and the
 * pin is switched by direct register writes to keep the timing exact. */
#define OW_BIT (1u << SENSOR_OW_PIN_NUM)

static void ow_mode(uint32_t mode, uint32_t pull) {
  GPIO_TypeDef *p = SENSOR_OW_PORT;
  p->PUPDR = (p->PUPDR & ~(3u << (2 * SENSOR_OW_PIN_NUM))) | (pull << (2 * SENSOR_OW_PIN_NUM));
  p->MODER = (p->MODER & ~(3u << (2 * SENSOR_OW_PIN_NUM))) | (mode << (2 * SENSOR_OW_PIN_NUM));
}
static void ow_low(void)     { SENSOR_OW_PORT->BRR = OW_BIT; ow_mode(1, 0); }
static void ow_high(void)    { SENSOR_OW_PORT->BSRR = OW_BIT; ow_mode(1, 0); }
static void ow_release(void) { ow_mode(0, 1); }
static int  ow_level(void)   { return (SENSOR_OW_PORT->IDR & OW_BIT) != 0; }

static void us_until(uint32_t t0, uint32_t us) {
  uint32_t n = us * (SystemCoreClock / 1000000u);
  while (DWT->CYCCNT - t0 < n) {}
}

static int ow_reset(void) {
  __disable_irq();
  uint32_t t = DWT->CYCCNT;
  ow_low();
  us_until(t, 480);
  ow_release();
  us_until(t, 480 + 70);
  int present = !ow_level();
  __enable_irq();
  us_until(t, 480 + 480);
  int released = ow_level();
  ow_high();
  return present && released;
}

static void ow_write_bit(int b) {
  __disable_irq();
  uint32_t t = DWT->CYCCNT;
  ow_low();
  us_until(t, b ? 5 : 65);
  ow_high();
  us_until(t, 75);
  __enable_irq();
}

static int ow_read_bit(void) {
  __disable_irq();
  uint32_t t = DWT->CYCCNT;
  ow_low();
  us_until(t, 3);
  ow_release();
  us_until(t, 13);
  int b = ow_level();
  us_until(t, 70);
  ow_high();
  us_until(t, 80);
  __enable_irq();
  return b;
}

static void ow_write(uint8_t v) { for (int i = 0; i < 8; i++) ow_write_bit((v >> i) & 1); }
static uint8_t ow_read(void) { uint8_t v = 0; for (int i = 0; i < 8; i++) v |= (uint8_t) (ow_read_bit() << i); return v; }

static uint8_t ow_crc8(const uint8_t *d, int n) {
  uint8_t crc = 0;
  for (int i = 0; i < n; i++) {
    uint8_t b = d[i];
    for (int k = 0; k < 8; k++) {
      uint8_t mix = (crc ^ b) & 1;
      crc >>= 1;
      if (mix) crc ^= 0x8C;
      b >>= 1;
    }
  }
  return crc;
}

static bool read_ds18b20(int16_t *temp_c100) {
  uint8_t sp[9];
  Board_UsInit();
  ow_release();
  HAL_Delay(2);
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!ow_reset()) continue;
    ow_write(0xCC); ow_write(0x44); /* Skip ROM, Convert T; bus stays driven high */
    Board_DelayMs(DS18B20_CONVERT_MS);
    if (!ow_reset()) continue;
    ow_write(0xCC); ow_write(0xBE); /* Skip ROM, Read Scratchpad */
    for (int i = 0; i < 9; i++) sp[i] = ow_read();
    if (ow_crc8(sp, 8) != sp[8]) continue;
    int16_t raw = (int16_t) ((sp[1] << 8) | sp[0]);
    if (raw == 0x0550) continue; /* 85.00 = power-on value, conversion did not run */
    *temp_c100 = (int16_t) ((int32_t) raw * 100 / 16);
    return true;
  }
  return false;
}

/* --------------------------------------------------------------- read all */
bool Sensor_ReadAll(uint8_t mode, sensor_reading_t *out) {
  out->moisture_mV = SENSOR_NO_MOISTURE;
  out->temp_c100 = SENSOR_NO_TEMP;
  out->ok = false;

  MX_ADC1_Init();
  out->vdda_mV = read_vdda_mV();
  if (mode == SENSOR_MODE_MOISTURE) {
    out->moisture_mV = read_moisture_mV(out->vdda_mV);
    out->ok = out->moisture_mV >= MOISTURE_MIN_MV;
  }
  HAL_ADC_DeInit(&hadc1); /* ADC back to deep power-down */

  if (mode == SENSOR_MODE_DS18B20) out->ok = read_ds18b20(&out->temp_c100);
  return out->ok;
}
