/* SX1262 driver glue and radio procedures. */
#include "board.h"
#include "app_hooks.h"
#include "bsp_radio.h"
#include "sx126x.h"
#include "sx126x_hal.h"

#define RADIO_IRQS (SX126X_IRQ_TX_DONE | SX126X_IRQ_RX_DONE | SX126X_IRQ_TIMEOUT | \
                    SX126X_IRQ_CRC_ERROR | SX126X_IRQ_HEADER_ERROR)
#define SPI_TIMEOUT_MS 100

typedef enum { RADIO_UNKNOWN, RADIO_SLEEPING, RADIO_STANDBY } radio_state_t;

static radio_state_t s_state = RADIO_UNKNOWN;
static int8_t s_tx_dbm = 10;
radio_timing_t g_radio_tm;

static void nss(int level) { HAL_GPIO_WritePin(LORA_NSS_PORT, LORA_NSS_PIN, level ? GPIO_PIN_SET : GPIO_PIN_RESET); }
static void rf_rx(int on)  { HAL_GPIO_WritePin(LORA_RFSW_PORT, LORA_RFSW_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET); }
static int  dio1(void)     { return HAL_GPIO_ReadPin(LORA_DIO1_PORT, LORA_DIO1_PIN) == GPIO_PIN_SET; }

static bool wait_busy(uint32_t timeout_ms) {
  uint32_t t0 = HAL_GetTick();
  while (HAL_GPIO_ReadPin(LORA_BUSY_PORT, LORA_BUSY_PIN) == GPIO_PIN_SET) {
    if (HAL_GetTick() - t0 > timeout_ms) return false;
  }
  return true;
}

/* ------------------------------------------- Semtech driver HAL interface */
sx126x_hal_status_t sx126x_hal_write(const void *ctx, const uint8_t *cmd, const uint16_t cmd_len,
                                     const uint8_t *data, const uint16_t data_len) {
  HAL_StatusTypeDef st;
  (void) ctx;
  if (!wait_busy(100)) return SX126X_HAL_STATUS_ERROR;
  nss(0);
  st = HAL_SPI_Transmit(&hspi1, (uint8_t *) cmd, cmd_len, SPI_TIMEOUT_MS);
  if (st == HAL_OK && data_len) st = HAL_SPI_Transmit(&hspi1, (uint8_t *) data, data_len, SPI_TIMEOUT_MS);
  nss(1);
  return st == HAL_OK ? SX126X_HAL_STATUS_OK : SX126X_HAL_STATUS_ERROR;
}

sx126x_hal_status_t sx126x_hal_read(const void *ctx, const uint8_t *cmd, const uint16_t cmd_len,
                                    uint8_t *data, const uint16_t data_len) {
  HAL_StatusTypeDef st;
  (void) ctx;
  if (!wait_busy(100)) return SX126X_HAL_STATUS_ERROR;
  nss(0);
  st = HAL_SPI_Transmit(&hspi1, (uint8_t *) cmd, cmd_len, SPI_TIMEOUT_MS);
  if (st == HAL_OK && data_len) {
    for (uint16_t i = 0; i < data_len; i++) data[i] = 0x00; /* clocked out as dummy bytes */
    st = HAL_SPI_Receive(&hspi1, data, data_len, SPI_TIMEOUT_MS);
  }
  nss(1);
  return st == HAL_OK ? SX126X_HAL_STATUS_OK : SX126X_HAL_STATUS_ERROR;
}

sx126x_hal_status_t sx126x_hal_reset(const void *ctx) {
  (void) ctx;
  HAL_GPIO_WritePin(LORA_RST_PORT, LORA_RST_PIN, GPIO_PIN_RESET);
  HAL_Delay(2);
  HAL_GPIO_WritePin(LORA_RST_PORT, LORA_RST_PIN, GPIO_PIN_SET);
  HAL_Delay(5);
  return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_wakeup(const void *ctx) {
  (void) ctx;
  nss(0);
  HAL_Delay(1);
  nss(1);
  return wait_busy(100) ? SX126X_HAL_STATUS_OK : SX126X_HAL_STATUS_ERROR;
}

/* ------------------------------------------------------------- procedures */
int Radio_Init(void) {
  int step = 0;
  sx126x_errors_mask_t errors = 0;
  static const sx126x_pa_cfg_params_t pa = { .pa_duty_cycle = 0x04, .hp_max = 0x07, .device_sel = 0x00, .pa_lut = 0x01 };
  static const sx126x_mod_params_lora_t mod = { .sf = SX126X_LORA_SF7, .bw = SX126X_LORA_BW_125,
                                                .cr = SX126X_LORA_CR_4_5, .ldro = 0 };
#define STEP(call) do { step++; if ((call) != SX126X_STATUS_OK) return step; } while (0)
  s_state = RADIO_UNKNOWN;
  nss(1);
  rf_rx(0);

  STEP(sx126x_reset(NULL));
  STEP(sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC));
  STEP(sx126x_set_dio3_as_tcxo_ctrl(NULL, SX126X_TCXO_CTRL_1_8V, 320)); /* module TCXO, 5 ms start-up */
  STEP(sx126x_clear_device_errors(NULL));
  STEP(sx126x_cal(NULL, SX126X_CAL_ALL));
  STEP(sx126x_set_reg_mode(NULL, SX126X_REG_MODE_DCDC));
  STEP(sx126x_set_dio2_as_rf_sw_ctrl(NULL, true));
  STEP(sx126x_set_pkt_type(NULL, SX126X_PKT_TYPE_LORA));
  STEP(sx126x_set_rf_freq(NULL, RADIO_FREQ_HZ));
  STEP(sx126x_cal_img_in_mhz(NULL, 863, 870));
  STEP(sx126x_set_pa_cfg(NULL, &pa));
  STEP(sx126x_set_tx_params(NULL, s_tx_dbm, SX126X_RAMP_200_US));
  STEP(sx126x_set_buffer_base_address(NULL, 0, 0));
  STEP(sx126x_set_lora_mod_params(NULL, &mod));
  STEP(sx126x_set_lora_sync_word(NULL, RADIO_SYNCWORD));
  STEP(sx126x_set_dio_irq_params(NULL, RADIO_IRQS, RADIO_IRQS, 0, 0));
  STEP(sx126x_get_device_errors(NULL, &errors));
#undef STEP
  if (errors) return 100; /* 0x20 = TCXO did not start */
  s_state = RADIO_STANDBY;
  return 0;
}

bool Radio_Wake(void) {
  if (s_state == RADIO_STANDBY) return true;
  /* Warm start: the configuration was kept in sleep. Error flags left from
   * before the sleep are cleared, not treated as a failure; if the radio is
   * really not working the next send fails and forces a cold start. */
  if (s_state == RADIO_SLEEPING
      && sx126x_wakeup(NULL) == SX126X_STATUS_OK
      && sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC) == SX126X_STATUS_OK
      && sx126x_clear_device_errors(NULL) == SX126X_STATUS_OK) {
    s_state = RADIO_STANDBY;
    return true;
  }
  return Radio_Init() == 0;
}

void Radio_Sleep(void) {
  if (s_state == RADIO_SLEEPING) return;
  /* state unknown (after a reset): bring the chip up first so that it
   * accepts the sleep command */
  if (s_state == RADIO_UNKNOWN && Radio_Init() != 0) return;
  rf_rx(0);
  if (sx126x_set_sleep(NULL, SX126X_SLEEP_CFG_WARM_START) == SX126X_STATUS_OK) {
    Board_DelayUs(600); /* no SPI access for 500 us after SetSleep */
    s_state = RADIO_SLEEPING;
  } else {
    s_state = RADIO_UNKNOWN;
  }
}

void Radio_SetTxPower(int8_t dbm) {
  if (dbm < -9) dbm = -9;
  if (dbm > 22) dbm = 22;
  if (dbm == s_tx_dbm) return;
  s_tx_dbm = dbm;
  if (s_state == RADIO_STANDBY) sx126x_set_tx_params(NULL, s_tx_dbm, SX126X_RAMP_200_US);
  else s_state = RADIO_UNKNOWN; /* applied by the next cold start */
}

int Radio_Send(const uint8_t *buf, uint8_t len, uint32_t timeout_ms) {
  const sx126x_pkt_params_lora_t pkt = { .preamble_len_in_symb = RADIO_PREAMBLE, .header_type = SX126X_LORA_PKT_EXPLICIT,
                                         .pld_len_in_bytes = len, .crc_is_on = true, .invert_iq_is_on = false };
  sx126x_irq_mask_t irq = 0;
  uint32_t t0, us0 = Board_Micros(), us1;

  if (!Radio_Wake()) return RADIO_ERR_SPI;
  rf_rx(0);
  if (sx126x_set_lora_pkt_params(NULL, &pkt) != SX126X_STATUS_OK) goto spi_error;
  if (sx126x_write_buffer(NULL, 0, buf, len) != SX126X_STATUS_OK) goto spi_error;
  sx126x_clear_irq_status(NULL, SX126X_IRQ_ALL);
  if (sx126x_set_tx(NULL, timeout_ms) != SX126X_STATUS_OK) goto spi_error;
  us1 = Board_Micros();
  g_radio_tm.tx_setup_us = us1 - us0;

  t0 = HAL_GetTick();
  while (!dio1()) {
    Board_Idle();
    if (HAL_GetTick() - t0 > timeout_ms + 500) {
      sx126x_get_and_clear_irq_status(NULL, &irq);
      s_state = RADIO_UNKNOWN;
      return (irq & SX126X_IRQ_TX_DONE) ? RADIO_ERR_NO_DIO1 : RADIO_ERR_NO_DONE;
    }
  }
  g_radio_tm.tx_air_us = Board_Micros() - us1;
  int ms = (int) (HAL_GetTick() - t0);
  sx126x_get_and_clear_irq_status(NULL, &irq);
  if (!(irq & SX126X_IRQ_TX_DONE)) { s_state = RADIO_UNKNOWN; return RADIO_ERR_TIMEOUT; }
  return ms;

spi_error:
  s_state = RADIO_UNKNOWN;
  return RADIO_ERR_SPI;
}

int Radio_Receive(uint8_t *buf, uint8_t max, uint32_t window_ms, int *rssi, int *snr) {
  const sx126x_pkt_params_lora_t pkt = { .preamble_len_in_symb = RADIO_PREAMBLE, .header_type = SX126X_LORA_PKT_EXPLICIT,
                                         .pld_len_in_bytes = 255, .crc_is_on = true, .invert_iq_is_on = false };
  sx126x_irq_mask_t irq = 0;
  uint32_t t0, us0 = Board_Micros(), us1;
  int ret = 0;

  if (!Radio_Wake()) return RADIO_ERR_SPI;
  if (sx126x_set_lora_pkt_params(NULL, &pkt) != SX126X_STATUS_OK) { s_state = RADIO_UNKNOWN; return RADIO_ERR_SPI; }
  sx126x_clear_irq_status(NULL, SX126X_IRQ_ALL);
  rf_rx(1); /* antenna to the receive path */
  if (sx126x_set_rx(NULL, window_ms) != SX126X_STATUS_OK) { rf_rx(0); s_state = RADIO_UNKNOWN; return RADIO_ERR_SPI; }

  us1 = Board_Micros();
  g_radio_tm.rx_setup_us = us1 - us0;
  t0 = HAL_GetTick();
  while (!dio1() && HAL_GetTick() - t0 < window_ms + 200) Board_Idle();
  g_radio_tm.rx_wait_us = Board_Micros() - us1;
  sx126x_get_and_clear_irq_status(NULL, &irq);
  if (irq & (SX126X_IRQ_CRC_ERROR | SX126X_IRQ_HEADER_ERROR)) {
    ret = RADIO_ERR_CRC;
  } else if (irq & SX126X_IRQ_RX_DONE) {
    sx126x_rx_buffer_status_t bs = {0};
    sx126x_pkt_status_lora_t ps = {0};
    sx126x_get_rx_buffer_status(NULL, &bs);
    sx126x_get_lora_pkt_status(NULL, &ps);
    uint8_t n = bs.pld_len_in_bytes > max ? max : bs.pld_len_in_bytes;
    sx126x_read_buffer(NULL, bs.buffer_start_pointer, buf, n);
    if (rssi) *rssi = ps.rssi_pkt_in_dbm;
    if (snr) *snr = ps.snr_pkt_in_db;
    ret = n;
  } else if (!(irq & SX126X_IRQ_TIMEOUT)) {
    sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC); /* DIO1 never fired */
  }
  rf_rx(0);
  return ret;
}
