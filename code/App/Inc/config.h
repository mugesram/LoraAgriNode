/*
 * Settings, kept in RAM and stored in flash sectors 0 and 1 (two copies,
 * the newer valid one wins, so a power cut during a save loses nothing).
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t magic;
  uint32_t version;         /* counts saves; the higher copy is the newer one */
  uint32_t period_s;        /* collection period */
  uint32_t max_age_s;       /* unsent records older than this are not sent (0 = no limit) */
  uint16_t device_id;
  uint16_t warmup_ms;       /* sensor power-on to reading */
  uint16_t ack_window_ms;   /* listen time after each LoRa packet */
  uint8_t  send_enable;     /* 1 = send over LoRa, 0 = only save to flash */
  uint8_t  recs_per_pkt;    /* LoRa transmit length, in records per packet */
  uint8_t  pkts_per_cycle;  /* most packets sent in one wake-up */
  int8_t   tx_dbm;
  uint8_t  sensor_mode;     /* SENSOR_MODE_x */
  uint8_t  reserved[3];
  uint32_t crc;
} config_t;

#define CFG_RECS_PER_PKT_MAX 15   /* 8-byte header + 15 x 16 bytes = 248 <= 255 */

extern config_t g_cfg;

bool Config_Load(void);                       /* false = defaults were used */
bool Config_Save(void);
void Config_Defaults(void);
/* text interface used by the USB commands */
bool Config_Set(const char *key, long value); /* false = unknown key or out of range */
int  Config_Count(void);
const char *Config_Key(int i);
long Config_Value(int i);
long Config_Min(int i);
long Config_Max(int i);

#endif
