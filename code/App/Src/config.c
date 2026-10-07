/* Settings storage. */
#include <stddef.h>
#include <string.h>
#include "bsp_flash.h"
#include "config.h"

#define CFG_MAGIC 0x4C434647u /* "LCFG" */

config_t g_cfg;

static uint32_t crc32(const void *data, uint32_t len) {
  const uint8_t *p = data;
  uint32_t crc = 0xFFFFFFFFu;
  while (len--) {
    crc ^= *p++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t) -(int32_t) (crc & 1));
  }
  return ~crc;
}

static bool copy_valid(const config_t *c) {
  return c->magic == CFG_MAGIC && c->crc == crc32(c, offsetof(config_t, crc));
}

/* ------------------------------------------------------------- key table */
typedef enum { T_U32, T_U16, T_U8, T_I8 } field_type_t;
typedef struct { const char *key; field_type_t type; uint16_t offset; long min, max, def; } field_t;

#define FIELD(name, type, min, max, def) { #name, type, offsetof(config_t, name), min, max, def }
static const field_t fields[] = {
  FIELD(period_s,       T_U32, 5,    86400,    60),
  FIELD(max_age_s,      T_U32, 0,    31536000, 259200), /* 3 days */
  FIELD(device_id,      T_U16, 1,    65535,    1),
  FIELD(warmup_ms,      T_U16, 0,    10000,    500),
  FIELD(ack_window_ms,  T_U16, 100,  3000,     300),  /* fixed reply time-out; a reply takes about 80 ms */
  FIELD(send_enable,    T_U8,  0,    1,        1),
  FIELD(recs_per_pkt,   T_U8,  1,    CFG_RECS_PER_PKT_MAX, 8),
  FIELD(pkts_per_cycle, T_U8,  1,    20,       3),
  FIELD(tx_dbm,         T_I8,  -9,   22,       10),
  FIELD(sensor_mode,    T_U8,  0,    1,        0),
};
#define NFIELDS ((int) (sizeof fields / sizeof fields[0]))

static void field_set(const field_t *f, long v) {
  void *p = (uint8_t *) &g_cfg + f->offset;
  switch (f->type) {
    case T_U32: *(uint32_t *) p = (uint32_t) v; break;
    case T_U16: *(uint16_t *) p = (uint16_t) v; break;
    case T_U8:  *(uint8_t *) p = (uint8_t) v; break;
    case T_I8:  *(int8_t *) p = (int8_t) v; break;
  }
}

static long field_get(const field_t *f) {
  const void *p = (const uint8_t *) &g_cfg + f->offset;
  switch (f->type) {
    case T_U32: return (long) *(const uint32_t *) p;
    case T_U16: return *(const uint16_t *) p;
    case T_U8:  return *(const uint8_t *) p;
    default:    return *(const int8_t *) p;
  }
}

int Config_Count(void) { return NFIELDS; }
const char *Config_Key(int i) { return fields[i].key; }
long Config_Value(int i) { return field_get(&fields[i]); }
long Config_Min(int i) { return fields[i].min; }
long Config_Max(int i) { return fields[i].max; }

bool Config_Set(const char *key, long value) {
  for (int i = 0; i < NFIELDS; i++) {
    if (strcmp(key, fields[i].key) != 0) continue;
    if (value < fields[i].min || value > fields[i].max) return false;
    field_set(&fields[i], value);
    return true;
  }
  return false;
}

void Config_Defaults(void) {
  memset(&g_cfg, 0, sizeof g_cfg);
  g_cfg.magic = CFG_MAGIC;
  for (int i = 0; i < NFIELDS; i++) field_set(&fields[i], fields[i].def);
}

/* a stored copy from older firmware may hold values that are out of range now */
static void clamp_all(void) {
  for (int i = 0; i < NFIELDS; i++) {
    long v = field_get(&fields[i]);
    if (v < fields[i].min || v > fields[i].max) field_set(&fields[i], fields[i].def);
  }
}

/* ------------------------------------------------------------ load / save */
bool Config_Load(void) {
  config_t c[2];
  bool ok[2];
  for (int i = 0; i < 2; i++)
    ok[i] = Flash_Read((uint32_t) i * W25Q_SECTOR_SIZE, &c[i], sizeof c[i]) && copy_valid(&c[i]);
  if (!ok[0] && !ok[1]) { Config_Defaults(); return false; }
  int pick = (ok[0] && (!ok[1] || c[0].version >= c[1].version)) ? 0 : 1;
  g_cfg = c[pick];
  clamp_all();
  return true;
}

bool Config_Save(void) {
  config_t c[2];
  bool ok[2];
  for (int i = 0; i < 2; i++)
    ok[i] = Flash_Read((uint32_t) i * W25Q_SECTOR_SIZE, &c[i], sizeof c[i]) && copy_valid(&c[i]);
  /* overwrite the older (or invalid) copy */
  int target = !ok[0] ? 0 : !ok[1] ? 1 : (c[0].version <= c[1].version ? 0 : 1);
  uint32_t newest = 0;
  for (int i = 0; i < 2; i++) if (ok[i] && c[i].version > newest) newest = c[i].version;

  g_cfg.magic = CFG_MAGIC;
  g_cfg.version = newest + 1;
  g_cfg.crc = crc32(&g_cfg, offsetof(config_t, crc));
  return Flash_EraseSector((uint32_t) target * W25Q_SECTOR_SIZE)
      && Flash_Write((uint32_t) target * W25Q_SECTOR_SIZE, &g_cfg, sizeof g_cfg);
}
