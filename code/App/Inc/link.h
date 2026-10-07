/*
 * LoRa link to the gateway.
 *
 * Data packet (node -> gateway), little-endian:
 *   0  magic 0xA5      1  type 1       2-3  device id
 *   4  record count    5  flags (bit0 = node clock is set)
 *   6-7 records still unsent after this packet
 *   8.. count x 16-byte records, exactly as stored in the flash
 * ACK packet (gateway -> node), 12 bytes:
 *   0  magic 0xA5      1  type 2       2-3  device id
 *   4-7 seq of the last record in the packet being acknowledged
 *   8-11 gateway time, Unix seconds (0 = gateway has no time)
 * Ping (node -> gateway), 4 bytes: magic, type 3, device id.
 * Pong (gateway -> node), 12 bytes: as the ACK with type 4 and seq 0.
 *
 * Each cycle starts with a ping. Data is sent only if the pong comes, so a
 * long data packet is never sent when nobody is listening.
 *
 * Reply time-out. The time from the end of the ping to the end of the pong
 * is measured, and twice that time is the time-out for every reply wait that
 * follows (the pong has the length of an ACK, so both take the same time).
 * Until a pong has been timed, the fixed time-out (setting ack_window_ms)
 * is used. After LINK_MISS_LIMIT replies missed in a row the calculated
 * value is dropped and the fixed time-out is used again until the next pong.
 */
#ifndef LINK_H
#define LINK_H

#include <stdbool.h>
#include <stdint.h>

#define LINK_MAGIC      0xA5
#define LINK_TYPE_DATA  1
#define LINK_TYPE_ACK   2
#define LINK_TYPE_PING  3
#define LINK_TYPE_PONG  4
#define LINK_TYPE_TEST  5   /* power-supply test builds: 24 bytes, seq in bytes 4-5, answered with an ACK */
#define LINK_HDR_LEN    8
#define LINK_ACK_LEN    12
#define LINK_PING_LEN   4

#define LINK_MISS_LIMIT   3    /* replies missed in a row before the calculated time-out is dropped */
#define LINK_CALC_MIN_MS  40   /* lowest calculated time-out */

typedef struct {
  uint32_t tx_ok;      /* packets sent */
  uint32_t tx_fail;    /* radio errors */
  uint32_t ack_ok;     /* replies heard (pong or ACK) */
  uint32_t ack_miss;   /* no (valid) reply heard */
  uint32_t reply_ms;   /* last measured ping-to-pong time, 0 = none yet */
  int      last_rssi;  /* of the last ACK, dBm */
  int      last_snr;
  int      last_error; /* last RADIO_ERR_x, 0 = none */
} link_stats_t;

extern link_stats_t g_link;

/* Pings the gateway, then sends unsent records, oldest first, until none are
 * left, the packet limit is reached, or a reply is missed (the records are
 * then left for the next cycle). Returns the number of records acknowledged. */
int Link_SendBacklog(void);

uint32_t Link_ReplyTimeoutMs(void);      /* time-out the next reply wait will use */
bool     Link_ReplyTimeoutIsCalc(void);  /* true = calculated from a ping, false = fixed */

#endif
