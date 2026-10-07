/* RTC time base on LSI, with rate correction. See timekeep.h. */
#include "board.h"
#include "timekeep.h"

#define BKP_MAGIC       0x54494D45u /* "TIME" */
#define RATE_ONE        (1u << 20)  /* rate is a Q20 fixed-point number */
#define RATE_MIN        ((uint32_t) (0.93 * RATE_ONE))  /* LSI 29.5-34 kHz gives 0.96-1.11 */
#define RATE_MAX        ((uint32_t) (1.14 * RATE_ONE))
#define RATE_LEARN_MIN_S 600        /* shortest interval between two syncs used to learn the rate */
#define RTC_NOMINAL_HZ  32768u      /* prescalers 128 x 256 */

static uint32_t s_ref_unix, s_ref_raw, s_rate, s_sync_unix, s_sync_raw, s_lsi_hz;
static bool s_valid, s_have_sync;
static volatile bool s_wake;

/* ---------------------------------------------------------------- storage */
static void state_save(void) {
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR1, s_ref_unix);
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR2, s_ref_raw);
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR3, s_rate);
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR4, (s_valid ? 1u : 0u) | (s_have_sync ? 2u : 0u));
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR5, s_sync_unix);
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR6, s_sync_raw);
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR7, s_lsi_hz);
}

static void state_load(void) {
  s_ref_unix = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR1);
  s_ref_raw = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR2);
  s_rate = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR3);
  uint32_t f = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR4);
  s_valid = (f & 1u) != 0;
  s_have_sync = (f & 2u) != 0;
  s_sync_unix = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR5);
  s_sync_raw = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR6);
  s_lsi_hz = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR7);
}

/* ------------------------------------------------------------ raw counter */
static void calendar_zero(void) {
  RTC_TimeTypeDef t = {0};
  RTC_DateTypeDef d = {0};
  t.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  t.StoreOperation = RTC_STOREOPERATION_RESET;
  d.WeekDay = RTC_WEEKDAY_SATURDAY; /* 2000-01-01 */
  d.Month = RTC_MONTH_JANUARY;
  d.Date = 1;
  d.Year = 0;
  HAL_RTC_SetTime(&hrtc, &t, RTC_FORMAT_BIN);
  HAL_RTC_SetDate(&hrtc, &d, RTC_FORMAT_BIN);
}

/* seconds since the calendar was zeroed (2000-01-01 00:00:00) */
uint32_t Time_Raw(void) {
  static const uint16_t before_month[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
  RTC_TimeTypeDef t;
  RTC_DateTypeDef d;
  HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN);
  HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN); /* must follow GetTime: unlocks the shadow registers */
  uint32_t y = d.Year, m = d.Month ? d.Month : 1;
  uint32_t days = y * 365u + (y + 3u) / 4u + before_month[(m - 1u) % 12u] + (d.Date ? d.Date - 1u : 0u);
  if (y % 4u == 0 && m > 2) days++;
  return days * 86400u + t.Hours * 3600u + t.Minutes * 60u + t.Seconds;
}

/* ----------------------------------------------------------- LSI measure */
/* TIM16 input capture 1 can be connected to LSI inside the chip. The timer
 * counts HSI16, one capture every 8 LSI periods. */
uint32_t Time_MeasureLsi(void) {
  enum { CAPTURES = 64 };
  TIM_HandleTypeDef htim = {0};
  TIM_IC_InitTypeDef ic = {0};
  uint32_t sum = 0, prev = 0;
  int got = 0;

  htim.Instance = TIM16;
  htim.Init.Prescaler = 0;
  htim.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim.Init.Period = 0xFFFF;
  htim.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim.Init.RepetitionCounter = 0;
  htim.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_IC_Init(&htim) != HAL_OK) return 0;
  HAL_TIMEx_RemapConfig(&htim, TIM_TIM16_TI1_LSI);
  ic.ICPolarity = TIM_ICPOLARITY_RISING;
  ic.ICSelection = TIM_ICSELECTION_DIRECTTI;
  ic.ICPrescaler = TIM_ICPSC_DIV8;
  ic.ICFilter = 0;
  HAL_TIM_IC_ConfigChannel(&htim, &ic, TIM_CHANNEL_1);
  HAL_TIM_IC_Start(&htim, TIM_CHANNEL_1);

  uint32_t t0 = HAL_GetTick();
  while (got <= CAPTURES && HAL_GetTick() - t0 < 100) {
    if (!__HAL_TIM_GET_FLAG(&htim, TIM_FLAG_CC1)) continue;
    uint32_t cap = HAL_TIM_ReadCapturedValue(&htim, TIM_CHANNEL_1); /* reading clears the flag */
    if (got > 0) sum += (uint16_t) (cap - prev);
    prev = cap;
    got++;
  }
  HAL_TIM_IC_Stop(&htim, TIM_CHANNEL_1);
  HAL_TIM_IC_DeInit(&htim);

  if (got <= CAPTURES || sum == 0) return 0;
  uint32_t hz = (uint32_t) ((uint64_t) SystemCoreClock * 8u * CAPTURES / sum);
  return (hz > 25000 && hz < 40000) ? hz : 0;
}

/* -------------------------------------------------------------------- API */
void Time_Init(void) {
  if (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0) == BKP_MAGIC) {
    state_load();
    if (s_rate >= RATE_MIN && s_rate <= RATE_MAX) return; /* RTC kept running through the reset */
  }
  /* power-up: start counting from zero */
  calendar_zero();
  s_lsi_hz = Time_MeasureLsi();
  s_rate = (uint32_t) (((uint64_t) RTC_NOMINAL_HZ << 20) / (s_lsi_hz ? s_lsi_hz : 32000u));
  s_ref_unix = 0;
  s_ref_raw = 0;
  s_valid = false;
  s_have_sync = false;
  s_sync_unix = 0;
  s_sync_raw = 0;
  state_save();
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, BKP_MAGIC);
}

uint32_t Time_Now(void) {
  uint32_t d = Time_Raw() - s_ref_raw;
  return s_ref_unix + (uint32_t) (((uint64_t) d * s_rate) >> 20);
}

bool Time_IsValid(void) { return s_valid; }
uint32_t Time_LsiHz(void) { return s_lsi_hz; }

int32_t Time_RatePpm(void) {
  return (int32_t) (((int64_t) s_rate - (int64_t) RATE_ONE) * 1000000 / (int64_t) RATE_ONE);
}

uint32_t Time_Set(uint32_t unix_s) {
  uint32_t raw = Time_Raw();
  uint32_t old = Time_Now();

  /* Two syncs far enough apart show how fast the RTC really runs. The
   * baseline moves only when the rate was learned, so frequent small
   * corrections (one per LoRa reply) still add up to a long interval. */
  if (!s_have_sync) {
    s_sync_unix = unix_s;
    s_sync_raw = raw;
    s_have_sync = true;
  } else if (raw - s_sync_raw >= RATE_LEARN_MIN_S) {
    if (unix_s > s_sync_unix) {
      uint64_t r = ((uint64_t) (unix_s - s_sync_unix) << 20) / (raw - s_sync_raw);
      if (r >= RATE_MIN && r <= RATE_MAX) s_rate = (uint32_t) r;
    }
    s_sync_unix = unix_s;
    s_sync_raw = raw;
  }
  s_ref_unix = unix_s;
  s_ref_raw = raw;
  s_valid = true;
  state_save();
  return old;
}

/* ------------------------------------------------------------ wake-up timer */
void Time_ArmWake(uint32_t seconds) {
  if (seconds < 1) seconds = 1;
  if (seconds > 65536) seconds = 65536;
  s_wake = false;
  HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, seconds - 1, RTC_WAKEUPCLOCK_CK_SPRE_16BITS);
}

bool Time_WakeFired(void) {
  bool w = s_wake;
  s_wake = false;
  return w;
}

void HAL_RTCEx_WakeUpTimerEventCallback(RTC_HandleTypeDef *h) {
  (void) h;
  s_wake = true;
}
