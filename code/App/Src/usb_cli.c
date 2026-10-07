/* USB command handler. See usb_cli.h for the command list. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "main.h"
#include "app.h"
#include "app_hooks.h"
#include "bsp_flash.h"
#include "bsp_sensor.h"
#include "bsp_usb.h"
#include "config.h"
#include "cycle.h"
#include "datalog.h"
#include "link.h"
#include "timekeep.h"
#include "timing.h"
#include "usb_cli.h"
#include "bsp_radio.h"
#ifdef POWER_TEST
#include "powertest.h"
#include "tlog.h"
#endif

static char s_line[96];
static unsigned s_len;

/* ------------------------------------------------------------------ output */
static bool reply(const char *fmt, ...) {
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
  va_end(ap);
  if (n < 0) return false;
  if (n > (int) sizeof buf - 3) n = sizeof buf - 3;
  buf[n++] = '\r';
  buf[n++] = '\n';
  return Usb_Write(buf, (uint32_t) n);
}

static void ok(void) { reply("OK"); }
static void err(const char *why) { reply("ERR %s", why); }

/* ---------------------------------------------------------------- commands */
static void cmd_id(void) {
  const uint32_t *uid = (const uint32_t *) UID_BASE;
  reply("board=%s", BOARD_NAME);
  reply("id=%u", (unsigned) g_cfg.device_id);
  reply("fw=%s", FW_VERSION);
  reply("proto=%d", PROTO_VERSION);
  reply("uid=%08lX%08lX%08lX", (unsigned long) uid[0], (unsigned long) uid[1], (unsigned long) uid[2]);
  ok();
}

static void cmd_status(void) {
  reply("time=%lu", (unsigned long) Time_Now());
  reply("time_valid=%d", Time_IsValid());
  reply("uptime_s=%lu", (unsigned long) Time_Raw());
  reply("period_s=%lu", (unsigned long) g_cfg.period_s);
  reply("next_cycle_s=%lu", (unsigned long) App_SecondsToNextCycle());
  reply("records=%lu", (unsigned long) Datalog_CountLive());
  reply("unsent=%lu", (unsigned long) Datalog_CountUnsent());
  reply("capacity=%lu", (unsigned long) LOG_CAPACITY);
  reply("used_permille=%lu", (unsigned long) Datalog_UsedPermille());
  reply("next_seq=%lu", (unsigned long) Datalog_NextSeq());
  reply("flash_ok=%d", g_health.flash_ok);
  reply("log_ok=%d", g_health.log_ok);
  reply("radio_ok=%d", g_health.radio_ok);
  reply("config_stored=%d", g_health.config_stored);
  reply("formatted=%d", g_health.formatted);
  reply("lsi_hz=%lu", (unsigned long) Time_LsiHz());
  reply("rtc_rate_ppm=%ld", (long) Time_RatePpm());
  reply("reset_flags=0x%08lX", (unsigned long) g_reset_flags);
  reply("cycles=%lu", (unsigned long) g_cycle.count);
  reply("last_time=%lu", (unsigned long) g_cycle.time);
  reply("last_sent=%d", g_cycle.sent);
  reply("last_saved=%d", g_cycle.saved);
  reply("last_sensor_ok=%d", g_cycle.reading.ok);
  reply("last_moisture_mV=%u", (unsigned) g_cycle.reading.moisture_mV);
  reply("last_temp_c100=%d", (int) g_cycle.reading.temp_c100);
  reply("last_vdda_mV=%u", (unsigned) g_cycle.reading.vdda_mV);
  reply("tx_ok=%lu", (unsigned long) g_link.tx_ok);
  reply("tx_fail=%lu", (unsigned long) g_link.tx_fail);
  reply("ack_ok=%lu", (unsigned long) g_link.ack_ok);
  reply("ack_miss=%lu", (unsigned long) g_link.ack_miss);
  reply("reply_ms=%lu", (unsigned long) g_link.reply_ms);
  reply("reply_timeout_ms=%lu", (unsigned long) Link_ReplyTimeoutMs());
  reply("reply_timeout_calc=%d", Link_ReplyTimeoutIsCalc());
  reply("ack_rssi=%d", g_link.last_rssi);
  reply("ack_snr=%d", g_link.last_snr);
  reply("radio_error=%d", g_link.last_error);
  ok();
}

static void cmd_sensor(void) {
  sensor_reading_t r;
  Sensor_PowerOn();
  Sensor_WaitWarmup(g_cfg.warmup_ms);
  Sensor_ReadAll(g_cfg.sensor_mode, &r);
  Sensor_PowerOff();
  reply("sensor_mode=%u", (unsigned) g_cfg.sensor_mode);
  reply("moisture_mV=%u", (unsigned) r.moisture_mV);
  reply("temp_c100=%d", (int) r.temp_c100);
  reply("vdda_mV=%u", (unsigned) r.vdda_mV);
  reply("sensor_ok=%d", r.ok);
  ok();
}

static void cmd_cfg_list(void) {
  for (int i = 0; i < Config_Count(); i++)
    reply("%s=%ld %ld %ld", Config_Key(i), Config_Value(i), Config_Min(i), Config_Max(i));
  ok();
}

static void apply_config(void) {
  Radio_SetTxPower(g_cfg.tx_dbm);
  App_Reschedule();
}

static void cmd_cfg_set(char *arg) {
  char *eq = strchr(arg, '=');
  char *end;
  if (!eq) { err("use CFG key=value"); return; }
  *eq = 0;
  long v = strtol(eq + 1, &end, 10);
  if (end == eq + 1) { err("value is not a number"); return; }
  if (!Config_Set(arg, v)) { err("unknown key or value out of range"); return; }
  if (!Config_Save()) { err("could not save to flash"); return; }
  apply_config();
  ok();
}

typedef struct { uint32_t count; bool aborted; } dump_ctx_t;

static bool dump_visit(uint32_t slot, const log_rec_t *r, void *ctx) {
  dump_ctx_t *d = ctx;
  bool sent_ok;
  (void) slot;
  switch (r->flags & LOG_F_TYPE_MASK) {
    case LOG_TYPE_DATA:
      /* R,seq,time,time_valid,moisture_mV,temp_c100,vdda_mV,sent */
      sent_ok = reply("R,%lu,%lu,%d,%u,%d,%u,%d", (unsigned long) r->seq, (unsigned long) r->time,
                      (r->flags & LOG_F_TIME_VALID) ? 1 : 0, (unsigned) r->moisture_mV, (int) r->temp_c100,
                      (unsigned) r->vdda_mV, (r->flags & LOG_F_UNSENT) ? 0 : 1);
      break;
    case LOG_TYPE_TIMECHG: {
      /* T,seq,new_time,old_time: the clock was changed at this point */
      uint32_t old;
      memcpy(&old, &r->moisture_mV, 4);
      sent_ok = reply("T,%lu,%lu,%lu", (unsigned long) r->seq, (unsigned long) r->time, (unsigned long) old);
      break;
    }
    default:
      return true;
  }
  if (!sent_ok) { d->aborted = true; return false; }
  d->count++;
  return true;
}

static void cmd_data(char *arg) {
  dump_ctx_t d = { 0, false };
  uint32_t from = 0, to = 0xFFFFFFFFu;
  char *end;
  if (*arg) {
    from = strtoul(arg, &end, 10);
    if (*end) to = strtoul(end, NULL, 10);
    if (to == 0) to = 0xFFFFFFFFu;
  }
  Datalog_ReadRange(from, to, dump_visit, &d);
  if (d.aborted) return;
  reply("END %lu", (unsigned long) d.count);
  ok();
}

static void cmd_erase(char *arg) {
  if (strcmp(arg, "ALL") == 0) {
    if (Datalog_DeleteAll()) ok(); else err("flash erase failed");
  } else if (strncmp(arg, "BEFORE ", 7) == 0) {
    uint32_t t = strtoul(arg + 7, NULL, 10);
    if (t == 0) { err("give a Unix time"); return; }
    reply("deleted=%lu", (unsigned long) Datalog_DeleteBefore(t));
    ok();
  } else {
    err("use ERASE ALL or ERASE BEFORE <unix>");
  }
}

static void cmd_timing(void) {
  for (int i = 0; i < TM_COUNT; i++)
    reply("%s_us=%lu", Timing_Name((timing_stage_t) i), (unsigned long) Timing_Us((timing_stage_t) i));
  ok();
}

#ifdef POWER_TEST
static void cmd_tlog(void) {
  uint32_t n = Tlog_Count();
  for (uint32_t i = 0; i < n; i++) {
    const tlog_rec_t *r = Tlog_Get(i);
    char buf[200];
    int len = snprintf(buf, sizeof buf, "L,%u,%u,%u,%lu", r->type, r->flags, r->seq, (unsigned long) r->time);
    for (int k = 0; k < TLOG_VALUES; k++) len += snprintf(buf + len, sizeof buf - (size_t) len, ",%lu", (unsigned long) r->v[k]);
    buf[len++] = '\r';
    buf[len++] = '\n';
    if (!Usb_Write(buf, (uint32_t) len)) return;
  }
  reply("END %lu %lu", (unsigned long) n, (unsigned long) Tlog_Capacity());
  ok();
}
#endif

static void run_line(char *line) {
  if (strcmp(line, "ID?") == 0) cmd_id();
  else if (strcmp(line, "STATUS?") == 0) cmd_status();
  else if (strcmp(line, "SENSOR?") == 0) cmd_sensor();
  else if (strncmp(line, "TIME=", 5) == 0) {
    uint32_t t = strtoul(line + 5, NULL, 10);
    if (t < 1600000000u) { err("give a Unix time"); return; }
    App_SetTime(t);
    ok();
  }
  else if (strcmp(line, "CFG?") == 0) cmd_cfg_list();
  else if (strncmp(line, "CFG ", 4) == 0) cmd_cfg_set(line + 4);
  else if (strcmp(line, "DEFAULTS") == 0) {
    uint16_t id = g_cfg.device_id;
    Config_Defaults();
    g_cfg.device_id = id;
    if (Config_Save()) { apply_config(); ok(); } else err("could not save to flash");
  }
  else if (strcmp(line, "DATA?") == 0) cmd_data(line + 5);
  else if (strncmp(line, "DATA? ", 6) == 0) cmd_data(line + 6);
  else if (strncmp(line, "ERASE ", 6) == 0) cmd_erase(line + 6);
  else if (strcmp(line, "TIMING?") == 0) cmd_timing();
  else if (strcmp(line, "CYCLE") == 0) { App_RequestCycle(); ok(); }
  else if (strcmp(line, "DFU") == 0) { ok(); Board_DelayMs(100); App_RebootToDfu(); }
#ifdef POWER_TEST
  else if (strcmp(line, "TLOG?") == 0) cmd_tlog();
  else if (strcmp(line, "TCLEAR") == 0) { if (Tlog_Clear()) ok(); else err("flash erase failed"); }
  else if (strcmp(line, "TRUN") == 0) { PowerTest_Round(); ok(); }
#endif
  else if (strcmp(line, "REBOOT") == 0) { ok(); Board_DelayMs(100); App_Reboot(); }
  else err("unknown command");
}

/* ------------------------------------------------------------------- input */
void UsbCli_Reset(void) { s_len = 0; }

void UsbCli_Task(void) {
  int c;
  while ((c = Usb_ReadChar()) >= 0) {
    if (c == '\r' || c == '\n') {
      if (s_len == 0) continue;
      s_line[s_len] = 0;
      s_len = 0;
      run_line(s_line);
    } else if (s_len < sizeof s_line - 1) {
      s_line[s_len++] = (char) c;
    }
  }
}
