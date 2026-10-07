/*
 * Timing log of the power-supply test builds, kept in the unused part of
 * the MCU's own flash (0x08010000 up). It is written while the board runs
 * from its test supply and read later over USB with the TLOG? command.
 */
#ifndef TLOG_H
#define TLOG_H

#include <stdbool.h>
#include <stdint.h>

#define TLOG_VALUES 14

typedef struct {
  uint8_t  type;              /* TLOG_x, never 0xFF */
  uint8_t  flags;
  uint16_t seq;
  uint32_t time;              /* Time_Now() when written */
  uint32_t v[TLOG_VALUES];
} tlog_rec_t;                 /* 64 bytes */

#define TLOG_FLASH   2        /* one round of the flash test */
#define TLOG_SETTLE  3        /* sensor settling: summary */
#define TLOG_CYCLE   4        /* one full collection cycle */
#define TLOG_TRACE   5        /* sensor settling: voltage at fixed times */

void Tlog_Init(void);
bool Tlog_Append(uint8_t type, uint8_t flags, uint16_t seq, const uint32_t *v);
uint32_t Tlog_Count(void);
uint32_t Tlog_Capacity(void);
const tlog_rec_t *Tlog_Get(uint32_t i);
bool Tlog_Clear(void);

#endif
