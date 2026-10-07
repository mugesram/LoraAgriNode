/* Power-supply test builds. See powertest.h. */
#ifdef POWER_TEST
#include <string.h>
#include "main.h"
#include "app.h"
#include "app_hooks.h"
#include "bsp_flash.h"
#include "bsp_radio.h"
#include "bsp_sensor.h"
#include "bsp_usb.h"
#include "config.h"
#include "cycle.h"
#include "link.h"
#include "powertest.h"
#include "timekeep.h"
#include "timing.h"
#include "tlog.h"

#ifndef TEST_PERIOD_MS
#define TEST_PERIOD_MS  2000   /* one round of the lora and cycle tests */
#endif
#ifndef TEST_TX_DBM
#define TEST_TX_DBM     22     /* full power: what a 2 km link with low antennas needs */
#endif
#define TEST_REPLY_MS   300    /* fixed reply time-out of the lora test */

static bool s_no_stop;         /* TRUN on USB: wait awake instead of Stop 2 */
static uint16_t s_seq;

/* Stop 2 for about ms. Unlike Power_EnterStop() it leaves the sensor rail,
 * flash and radio in the state the test put them in. */
static void stop_ms(uint32_t ms) {
  if (s_no_stop) { Board_DelayMs(ms); return; }
  uint32_t hz = Time_LsiHz();
  if (hz == 0) hz = 32000;
  uint32_t counts = (uint32_t) ((uint64_t) ms * (hz / 16u) / 1000u);
  if (counts < 2) counts = 2;
  if (counts > 0x10000u) counts = 0x10000u;
  HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, counts - 1, RTC_WAKEUPCLOCK_RTCCLK_DIV16);
  CoreDebug->DEMCR &= ~CoreDebug_DEMCR_TRCENA_Msk;
  HAL_SuspendTick();
  HAL_PWREx_EnterSTOP2Mode(PWR_STOPENTRY_WFI);
  HAL_ResumeTick();
  Board_UsInit();
  HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);
}

/* rest of the period after a round that started at tick t0 */
__attribute__((unused)) static void stop_rest(uint32_t t0, uint32_t period_ms) {
  uint32_t used = HAL_GetTick() - t0;
  if (used + 5 < period_ms) stop_ms(period_ms - used);
}

/* ------------------------------------------------------------------- lora */
#if POWER_TEST == PT_LORA
#define NVAL 9
static uint16_t s_prev[NVAL];   /* timing of the round before, in 10 us steps */

static uint16_t u10(uint32_t us) { us /= 10u; return us > 0xFFFEu ? 0xFFFEu : (uint16_t) us; }

static bool reply_ok(const uint8_t *rx, int len, uint8_t type, uint32_t seq) {
  uint32_t got;
  if (len < LINK_ACK_LEN || rx[0] != LINK_MAGIC || rx[1] != type) return false;
  if ((uint16_t) (rx[2] | (rx[3] << 8)) != g_cfg.device_id) return false;
  memcpy(&got, rx + 4, 4);
  return got == seq;
}

static void test_start(void) {
  Sensor_PowerOff();
  Flash_Sleep();
  Radio_SetTxPower(TEST_TX_DBM);
  memset(s_prev, 0, sizeof s_prev);
  s_seq = 0;
}

static void test_round(void) {
  uint16_t cur[NVAL];
  uint8_t pkt[LINK_HDR_LEN + 16], rx[32];
  uint32_t t0 = HAL_GetTick(), a;
  int n;
  memset(cur, 0xFF, sizeof cur);   /* 0xFFFF = did not happen */

  a = Board_Micros();
  bool up = Radio_Wake();
  cur[0] = u10(Board_Micros() - a);
  if (up) {
    /* ask: ping, wait for the pong */
    pkt[0] = LINK_MAGIC; pkt[1] = LINK_TYPE_PING;
    pkt[2] = (uint8_t) g_cfg.device_id; pkt[3] = (uint8_t) (g_cfg.device_id >> 8);
    if (Radio_Send(pkt, LINK_PING_LEN, 2000) >= 0) {
      cur[1] = u10(g_radio_tm.tx_setup_us);
      cur[2] = u10(g_radio_tm.tx_air_us);
      n = Radio_Receive(rx, sizeof rx, TEST_REPLY_MS, NULL, NULL);
      cur[3] = u10(g_radio_tm.rx_setup_us);
      if (reply_ok(rx, n, LINK_TYPE_PONG, 0)) cur[4] = u10(g_radio_tm.rx_wait_us);
    }
    /* data: a packet of the size of a real one-reading packet (24 bytes),
     * filled with the timing of the round before */
    pkt[1] = LINK_TYPE_TEST;
    pkt[4] = (uint8_t) s_seq; pkt[5] = (uint8_t) (s_seq >> 8);
    memcpy(pkt + 6, s_prev, sizeof s_prev);
    if (Radio_Send(pkt, sizeof pkt, 2000) >= 0) {
      cur[5] = u10(g_radio_tm.tx_setup_us);
      cur[6] = u10(g_radio_tm.tx_air_us);
      n = Radio_Receive(rx, sizeof rx, TEST_REPLY_MS, NULL, NULL);
      cur[7] = u10(g_radio_tm.rx_setup_us);
      if (reply_ok(rx, n, LINK_TYPE_ACK, s_seq)) cur[8] = u10(g_radio_tm.rx_wait_us);
    }
  }
  Radio_Sleep();
  memcpy(s_prev, cur, sizeof s_prev);
  s_seq++;
  stop_rest(t0, TEST_PERIOD_MS);
}

/* ------------------------------------------------------------------ flash */
#elif POWER_TEST == PT_FLASH
#define PHASE_MS       10000u
#define SCRATCH_BASE   0x0F0000u   /* last 16 sectors of the W25Q80 */
#define SCRATCH_WRITE  15u         /* sectors used by the write phase */
#define REC_SECTOR     (SCRATCH_BASE + 15u * W25Q_SECTOR_SIZE)   /* 16-byte record writes */

typedef struct { uint32_t n, sum, min, max; } stat_t;
static void stat_add(stat_t *s, uint32_t us) {
  if (s->n == 0 || us < s->min) s->min = us;
  if (us > s->max) s->max = us;
  s->sum += us;
  s->n++;
}
static uint32_t stat_avg(const stat_t *s) { return s->n ? s->sum / s->n : 0; }

static uint8_t s_page[W25Q_PAGE_SIZE];
static uint32_t s_sector, s_rec;

static void test_start(void) {
  Sensor_PowerOff();
  Radio_Sleep();
  Flash_Wake();
  Flash_EraseSector(REC_SECTOR);
  s_sector = s_rec = 0;
  s_seq = 0;
}

static void test_round(void) {
  stat_t rd = {0}, pg = {0}, er = {0};
  uint32_t v[TLOG_VALUES] = {0}, errors = 0, t0, a, addr = SCRATCH_BASE;

  /* 1. reading, 256 bytes at a time */
  a = Board_Micros();
  Flash_Wake();
  v[12] = Board_Micros() - a;
  t0 = HAL_GetTick();
  while (HAL_GetTick() - t0 < PHASE_MS) {
    a = Board_Micros();
    if (!Flash_Read(addr, s_page, sizeof s_page)) errors++;
    stat_add(&rd, Board_Micros() - a);
    addr += W25Q_PAGE_SIZE;
    if (addr >= SCRATCH_BASE + 16u * W25Q_SECTOR_SIZE) addr = SCRATCH_BASE;
    if ((rd.n & 63u) == 0) Board_Idle();
  }

  /* 2. writing: erase a sector, then program its 16 pages (each checked by
   *    a read-back inside Flash_Write) */
  for (uint32_t i = 0; i < sizeof s_page; i++) s_page[i] = (uint8_t) (i * 7u + s_seq);
  t0 = HAL_GetTick();
  while (HAL_GetTick() - t0 < PHASE_MS) {
    uint32_t base = SCRATCH_BASE + (s_sector++ % SCRATCH_WRITE) * W25Q_SECTOR_SIZE;
    a = Board_Micros();
    if (!Flash_EraseSector(base)) errors++;
    stat_add(&er, Board_Micros() - a);
    for (uint32_t p = 0; p < W25Q_SECTOR_SIZE / W25Q_PAGE_SIZE && HAL_GetTick() - t0 < PHASE_MS; p++) {
      a = Board_Micros();
      if (!Flash_Write(base + p * W25Q_PAGE_SIZE, s_page, sizeof s_page)) errors++;
      stat_add(&pg, Board_Micros() - a);
    }
    Board_Idle();
  }

  /* one 16-byte write, as the data log does for each reading */
  if (s_rec >= W25Q_SECTOR_SIZE / 16u) { Flash_EraseSector(REC_SECTOR); s_rec = 0; }
  a = Board_Micros();
  if (!Flash_Write(REC_SECTOR + 16u * s_rec++, s_page + 1, 16)) errors++;
  v[13] = Board_Micros() - a;

  /* 3. power-down */
  a = Board_Micros();
  Flash_Sleep();
  v[11] = Board_Micros() - a;

  v[0] = rd.n; v[1] = stat_avg(&rd); v[2] = rd.min; v[3] = rd.max;
  v[4] = pg.n; v[5] = stat_avg(&pg); v[6] = pg.min; v[7] = pg.max;
  v[8] = er.n; v[9] = stat_avg(&er); v[10] = er.max;
  Tlog_Append(TLOG_FLASH, errors > 255u ? 255u : (uint8_t) errors, s_seq++, v);

  for (uint32_t i = 0; i < PHASE_MS / 2000u; i++) { stop_ms(2000); Board_Idle(); }
}

/* ----------------------------------------------------------------- sensor */
#elif POWER_TEST == PT_SENSOR
#define SETTLE_RUNS   3
#define TRACE_MS      2000     /* one sample each millisecond */
#define OFF_MS        3000     /* rail off before each measurement */

static uint16_t s_trace[TRACE_MS];
static uint32_t s_runs;

static uint16_t adc_once(void) {
  if (HAL_ADC_Start(&hadc1) != HAL_OK || HAL_ADC_PollForConversion(&hadc1, 10) != HAL_OK) return 0;
  return (uint16_t) HAL_ADC_GetValue(&hadc1);
}

/* first millisecond from which the voltage stays within +-pct % of final */
static uint32_t settled_ms(uint32_t final, uint32_t pct) {
  uint32_t band = final * pct / 100u;
  if (band == 0) band = 1;
  for (int i = TRACE_MS - 1; i >= 0; i--) {
    uint32_t d = s_trace[i] > final ? s_trace[i] - final : final - s_trace[i];
    if (d > band) return (uint32_t) i + 1u;
  }
  return 0;
}

static void settle_measure(void) {
  static const uint16_t at_ms[TLOG_VALUES] = { 1, 2, 5, 10, 20, 50, 100, 200, 300, 500, 750, 1000, 1500, 1999 };
  ADC_ChannelConfTypeDef ch = {0};
  GPIO_InitTypeDef g = {0};
  sensor_reading_t r;
  uint32_t v[TLOG_VALUES] = {0}, t0, sum = 0, lo = 0xFFFF, hi = 0, peak = 0, peak_at = 0;

  Sensor_PowerOff();
  stop_ms(OFF_MS);
  Sensor_ReadAll(SENSOR_MODE_MOISTURE, &r);   /* for the supply voltage; leaves the ADC off */
  uint32_t vdda = r.vdda_mV ? r.vdda_mV : 3300u;

  g.Pin = SENSOR_ANA_PIN;
  g.Mode = GPIO_MODE_ANALOG;
  g.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(SENSOR_ANA_PORT, &g);
  MX_ADC1_Init();
  ch.Channel = SENSOR_ANA_CHANNEL;
  ch.Rank = ADC_REGULAR_RANK_1;
  ch.SamplingTime = ADC_SAMPLETIME_640CYCLES_5;   /* as in the real reading */
  ch.SingleDiff = ADC_SINGLE_ENDED;
  ch.OffsetNumber = ADC_OFFSET_NONE;
  HAL_ADC_ConfigChannel(&hadc1, &ch);
  adc_once();
  uint32_t v_off = adc_once() * vdda / 4095u;

  Sensor_PowerOn();
  t0 = Board_Micros();
  for (uint32_t i = 0; i < TRACE_MS; i++) {
    while (Board_Micros() - t0 < i * 1000u) {}
    s_trace[i] = (uint16_t) (adc_once() * vdda / 4095u);
    if ((i & 127u) == 0) HAL_IWDG_Refresh(&hiwdg);
  }
  HAL_ADC_Stop(&hadc1);
  HAL_ADC_DeInit(&hadc1);

  for (uint32_t i = TRACE_MS - 100; i < TRACE_MS; i++) {
    sum += s_trace[i];
    if (s_trace[i] < lo) lo = s_trace[i];
    if (s_trace[i] > hi) hi = s_trace[i];
  }
  uint32_t final = sum / 100u;
  for (uint32_t i = 0; i < TRACE_MS; i++) if (s_trace[i] > peak) { peak = s_trace[i]; peak_at = i; }

  v[0] = vdda; v[1] = v_off; v[2] = final;
  v[3] = settled_ms(final, 10); v[4] = settled_ms(final, 5); v[5] = settled_ms(final, 2); v[6] = settled_ms(final, 1);
  v[7] = peak; v[8] = peak_at; v[9] = hi - lo;   /* ripple in the last 100 ms */
  Tlog_Append(TLOG_SETTLE, 0, s_seq, v);
  for (int i = 0; i < TLOG_VALUES; i++) v[i] = s_trace[at_ms[i]];
  Tlog_Append(TLOG_TRACE, 0, s_seq++, v);
}

static void test_start(void) {
  Flash_Sleep();
  Radio_Sleep();
  s_runs = 0;
  s_seq = 0;
}

static void test_round(void) {
  if (s_runs < SETTLE_RUNS || s_no_stop) { settle_measure(); s_runs++; return; }
  Sensor_PowerOn();   /* from here on the rail simply stays on */
  stop_ms(2000);
}

/* ------------------------------------------------------------------ cycle */
#elif POWER_TEST == PT_CYCLE
static void test_start(void) {
  Radio_SetTxPower(TEST_TX_DBM);
  s_seq = 0;
}

static void test_round(void) {
  uint32_t v[TLOG_VALUES] = {0}, t0 = HAL_GetTick();
  Cycle_Run();
  for (int i = 0; i < TM_COUNT; i++) v[i] = Timing_Us((timing_stage_t) i);
  v[9] = (uint32_t) g_cycle.sent;
  v[10] = g_cycle.reading.moisture_mV;
  v[11] = g_cycle.reading.vdda_mV;
  v[12] = Link_ReplyTimeoutMs();
  v[13] = g_link.reply_ms;
  Tlog_Append(TLOG_CYCLE, (uint8_t) ((g_cycle.saved ? 1 : 0) | (Link_ReplyTimeoutIsCalc() ? 2 : 0)), s_seq++, v);
  /* as Power_EnterStop() does before the real sleep */
  Sensor_PowerOff();
  Flash_Sleep();
  Radio_Sleep();
  stop_rest(t0, TEST_PERIOD_MS);
}
#else
#error "POWER_TEST must be PT_LORA, PT_FLASH, PT_SENSOR or PT_CYCLE"
#endif

/* ---------------------------------------------------------------- running */
void PowerTest_Round(void) {
  /* the lora test reports a round in the packet of the next one */
  int rounds = POWER_TEST == PT_LORA || POWER_TEST == PT_CYCLE ? 3 : 1;
  s_no_stop = true;
  test_start();
  while (rounds--) test_round();
  s_no_stop = false;
}

void PowerTest_Run(void) {
  bool started = false;
  App_Init();
  Tlog_Init();
  for (;;) {
    Board_Idle();
    if (App_UsbService()) { started = false; continue; }   /* on the PC: commands only */
    if (!started) { test_start(); started = true; }
    test_round();
  }
}
#endif /* POWER_TEST */
