/*
 * Application main loop.
 *
 * Normal mode: sleep in Stop 2, wake on the RTC timer, run a collection
 * cycle when one is due. The sleep is cut into short pieces so that the
 * watchdog is refreshed and a USB plug-in is noticed within a few seconds.
 *
 * USB mode: entered when a PC is found on the cable. The MCU stays awake
 * and answers commands. No collection cycle runs on the timer in this mode
 * (only the CYCLE command starts one); the period starts again when USB is
 * switched off.
 */
#include "main.h"
#include "app.h"
#include "app_hooks.h"
#include "bsp_flash.h"
#include "bsp_radio.h"
#include "bsp_sensor.h"
#include "bsp_usb.h"
#include "config.h"
#include "cycle.h"
#include "datalog.h"
#include "power.h"
#include "timekeep.h"
#include "usb_cli.h"

#define SLEEP_CHUNK_S        2      /* longest single sleep */
#define USB_LINK_TIMEOUT_MS  5000   /* leave USB mode when the host is gone this long */
#define USB_RETRY_S          30     /* wait before trying a silent host again */
#define FIRST_CYCLE_DELAY_S  2

app_health_t g_health;

static uint32_t s_next_due;       /* time of the next collection cycle */
static bool s_cycle_now;
static bool s_usb_blocked;        /* last USB session ended without an unplug */
static uint32_t s_usb_retry_at;
static uint32_t s_usb_alive_tick;
static bool s_usb_was_linked;     /* a PC enumerated the board in this USB session */

void Board_Idle(void) {
  HAL_IWDG_Refresh(&hiwdg);
  Usb_Task();
}

/* a device id that differs between boards, used until one is set */
static uint16_t default_device_id(void) {
  const uint32_t *uid = (const uint32_t *) UID_BASE;
  uint32_t x = uid[0] ^ uid[1] ^ uid[2];
  uint16_t id = (uint16_t) (x ^ (x >> 16));
  return id ? id : 1;
}

void App_Init(void) {
  g_health.flash_ok = Flash_Init();
  /* read twice before deciding that there are no stored settings */
  g_health.config_stored = g_health.flash_ok && (Config_Load() || Config_Load());
  if (!g_health.config_stored) {
    Config_Defaults();
    g_cfg.device_id = default_device_id();
  }
  if (g_health.flash_ok && !g_health.config_stored) {
    /* No valid settings: this chip was never used by this firmware (the
     * board-test firmware filled it with test data). Erase the data ring
     * and store the defaults. Takes about 12 s on a full chip. */
    g_health.formatted = true;
    g_health.log_ok = Datalog_Format();
    g_health.config_stored = Config_Save();
  } else {
    g_health.log_ok = g_health.flash_ok && Datalog_Init();
  }

  Time_Init();

  Radio_SetTxPower(g_cfg.tx_dbm);
  g_health.radio_ok = Radio_Init() == 0;
  Radio_Sleep();
  Flash_Sleep();

  s_next_due = Time_Now() + FIRST_CYCLE_DELAY_S;
}

void App_SetTime(uint32_t unix_s) {
  bool was_valid = Time_IsValid();
  uint32_t old = Time_Set(unix_s);
  /* keep the distance to the next cycle */
  s_next_due += unix_s - old;
  if (!was_valid || old != unix_s) Datalog_AppendTimeChange(old, unix_s);
}

void App_Reschedule(void) { s_next_due = Time_Now() + g_cfg.period_s; }
void App_RequestCycle(void) { s_cycle_now = true; }

uint32_t App_SecondsToNextCycle(void) {
  uint32_t now = Time_Now();
  return s_next_due > now ? s_next_due - now : 0;
}

void App_Reboot(void) {
  Usb_Stop();
  NVIC_SystemReset();
}

void App_RebootToDfu(void) {
  Usb_Stop();
  g_boot_request = BOOT_REQUEST_DFU;
  NVIC_SystemReset();
}

/* ---------------------------------------------------------------- USB mode */
bool App_UsbService(void) {
  if (Usb_IsActive()) {
    UsbCli_Task();
    if (Usb_IsLinked()) {
      s_usb_alive_tick = HAL_GetTick();
      s_usb_was_linked = true;
    } else if (HAL_GetTick() - s_usb_alive_tick > USB_LINK_TIMEOUT_MS) {
      /* unplugged, or the PC went to sleep */
      Usb_Stop();
      s_usb_blocked = true;
      s_usb_retry_at = Time_Now() + USB_RETRY_S;
      /* a full period from now; not after a silent host (PC asleep), which
       * is tried again every USB_RETRY_S and would hold the cycle off for ever */
      if (s_usb_was_linked) App_Reschedule();
    }
    return Usb_IsActive();
  }
  if (!Usb_HostPresent()) {
    s_usb_blocked = false;
    return false;
  }
  if (s_usb_blocked && Time_Now() < s_usb_retry_at) return false;
  Usb_Start();
  UsbCli_Reset();
  s_usb_alive_tick = HAL_GetTick();
  s_usb_was_linked = false;
  return true;
}

/* --------------------------------------------------------------- main loop */
void App_Run(void) {
  Board_Idle();
  App_UsbService();

  if (s_cycle_now || (!Usb_IsActive() && Time_Now() >= s_next_due)) {
    bool forced = s_cycle_now;
    s_cycle_now = false;
    Cycle_Run();
    if (!forced) s_next_due += g_cfg.period_s;
    /* never schedule in the past (long cycle, or the clock was set forward) */
    uint32_t now = Time_Now();
    if (s_next_due <= now) s_next_due = now + g_cfg.period_s;
  }

  if (!Usb_IsActive()) {
    uint32_t wait = App_SecondsToNextCycle();
    if (wait == 0) return;
    if (wait > SLEEP_CHUNK_S) wait = SLEEP_CHUNK_S;
    Time_ArmWake(wait);
    Power_EnterStop();
  }
}
