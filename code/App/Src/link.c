/* LoRa packets and ACK handling. See link.h for the formats. */
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "board.h"
#include "bsp_radio.h"
#include "config.h"
#include "datalog.h"
#include "link.h"
#include "timekeep.h"
#include "timing.h"

#define TX_TIMEOUT_MS    2000
#define TIME_STEP_MIN_S  2     /* smaller differences are not corrected */

link_stats_t g_link;

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }
static uint16_t get16(const uint8_t *p) { return (uint16_t) (p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint8_t build_packet(uint8_t *buf, const log_rec_t *recs, int count) {
  uint32_t unsent = Datalog_CountUnsent();
  uint32_t remaining = unsent > (uint32_t) count ? unsent - (uint32_t) count : 0;
  buf[0] = LINK_MAGIC;
  buf[1] = LINK_TYPE_DATA;
  put16(buf + 2, g_cfg.device_id);
  buf[4] = (uint8_t) count;
  buf[5] = Time_IsValid() ? 1 : 0;
  put16(buf + 6, remaining > 0xFFFF ? 0xFFFF : (uint16_t) remaining);
  memcpy(buf + LINK_HDR_LEN, recs, (size_t) count * sizeof(log_rec_t));
  return (uint8_t) (LINK_HDR_LEN + count * (int) sizeof(log_rec_t));
}

/* true if buf is the reply of this type for us (ACK: seq of the last record
 * of the packet, pong: 0) */
static bool parse_reply(const uint8_t *buf, int len, uint8_t type, uint32_t seq, uint32_t *gw_time) {
  if (len < LINK_ACK_LEN || buf[0] != LINK_MAGIC || buf[1] != type) return false;
  if (get16(buf + 2) != g_cfg.device_id || get32(buf + 4) != seq) return false;
  *gw_time = get32(buf + 8);
  return true;
}

static void sync_time(uint32_t gw_time);

/* ---------------------------------------------------------- reply time-out */
static uint16_t s_calc_ms;   /* calculated from the last pong, 0 = none: use the fixed one */
static uint8_t  s_misses;    /* replies missed in a row */

uint32_t Link_ReplyTimeoutMs(void) { return s_calc_ms ? s_calc_ms : g_cfg.ack_window_ms; }
bool Link_ReplyTimeoutIsCalc(void) { return s_calc_ms != 0; }

/* Listens for one reply. Returns true if it came; *took_ms is then the time
 * from the start of listening to the end of the reply. */
static bool wait_reply(uint8_t type, uint32_t seq, uint32_t *gw_time, uint32_t *took_ms) {
  uint8_t rx[32];
  int rssi = 0, snr = 0;
  uint32_t t0 = Board_Micros();
  int rlen = Radio_Receive(rx, sizeof rx, Link_ReplyTimeoutMs(), &rssi, &snr);
  uint32_t us = Board_Micros() - t0;
  Timing_Add(TM_LORA_ACK, us);
  if (rlen <= 0 || !parse_reply(rx, rlen, type, seq, gw_time)) {
    g_link.ack_miss++;
    if (rlen < 0) g_link.last_error = rlen;
    if (++s_misses >= LINK_MISS_LIMIT) { s_calc_ms = 0; s_misses = 0; }
    return false;
  }
  s_misses = 0;
  g_link.ack_ok++;
  g_link.last_rssi = rssi;
  g_link.last_snr = snr;
  g_link.last_error = 0;
  *took_ms = (us + 999u) / 1000u;
  return true;
}

/* Asks the gateway whether it is there. The answer also gives the reply
 * time, from which the time-out of the following waits is worked out. */
static bool ping(void) {
  uint8_t pkt[LINK_PING_LEN] = { LINK_MAGIC, LINK_TYPE_PING };
  uint32_t gw_time = 0, took_ms = 0;
  put16(pkt + 2, g_cfg.device_id);
  int air_ms = Radio_Send(pkt, sizeof pkt, TX_TIMEOUT_MS);
  if (air_ms < 0) { g_link.tx_fail++; g_link.last_error = air_ms; return false; }
  g_link.tx_ok++;
  Timing_Add(TM_LORA_TX, (uint32_t) air_ms * 1000u);
  if (!wait_reply(LINK_TYPE_PONG, 0, &gw_time, &took_ms)) return false;

  uint32_t calc = 2u * took_ms;
  if (calc < LINK_CALC_MIN_MS) calc = LINK_CALC_MIN_MS;
  if (calc > g_cfg.ack_window_ms) calc = g_cfg.ack_window_ms;
  s_calc_ms = (uint16_t) calc;
  g_link.reply_ms = took_ms;
  sync_time(gw_time);
  return true;
}

static void sync_time(uint32_t gw_time) {
  if (gw_time == 0) return;
  uint32_t now = Time_Now();
  uint32_t diff = now > gw_time ? now - gw_time : gw_time - now;
  if (!Time_IsValid() || diff >= TIME_STEP_MIN_S) App_SetTime(gw_time);
}

int Link_SendBacklog(void) {
  static log_rec_t recs[CFG_RECS_PER_PKT_MAX];
  static uint8_t pkt[RADIO_MAX_PAYLOAD];
  uint32_t slots[CFG_RECS_PER_PKT_MAX];
  int acked = 0;

  if (!ping()) return 0; /* nobody answers: keep the records for the next cycle */

  for (int p = 0; p < g_cfg.pkts_per_cycle; p++) {
    int n = Datalog_GetUnsent(recs, slots, g_cfg.recs_per_pkt, Time_Now(), Time_IsValid(), g_cfg.max_age_s);
    if (n <= 0) break;

    uint8_t len = build_packet(pkt, recs, n);
    int air_ms = Radio_Send(pkt, len, TX_TIMEOUT_MS);
    if (air_ms < 0) { g_link.tx_fail++; g_link.last_error = air_ms; break; }
    g_link.tx_ok++;
    Timing_Add(TM_LORA_TX, (uint32_t) air_ms * 1000u);

    uint32_t gw_time = 0, took_ms = 0;
    /* not acknowledged: leave the records unsent, they go next time */
    if (!wait_reply(LINK_TYPE_ACK, recs[n - 1].seq, &gw_time, &took_ms)) break;
    for (int i = 0; i < n; i++) Datalog_MarkSent(slots[i]);
    acked += n;
    sync_time(gw_time);
  }
  return acked;
}
