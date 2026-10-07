/*
 * Data log: a ring of 16-byte records in the external flash.
 *
 * Flash map (4 KB sectors): 0-1 settings, 2-255 data ring.
 * Records are written one after the other. A sector is erased when the
 * write position enters it, so the oldest 256 records go when the ring is
 * full. "Sent" and "deleted" are marked by clearing a bit inside the record
 * (flash can change a bit from 1 to 0 without an erase), so no separate
 * pointer has to survive a power cut.
 */
#ifndef DATALOG_H
#define DATALOG_H

#include <stdbool.h>
#include <stdint.h>

#define LOG_FIRST_SECTOR     2u
#ifndef LOG_SECTORS
#define LOG_SECTORS          254u
#endif
#define LOG_RECS_PER_SECTOR  256u
#define LOG_CAPACITY         (LOG_SECTORS * LOG_RECS_PER_SECTOR)

/* flags, low nibble: may be cleared later, not covered by the CRC */
#define LOG_F_UNSENT      0x01u   /* 1 = still to be sent over LoRa */
#define LOG_F_LIVE        0x02u   /* 1 = not deleted */
#define LOG_F_MUTABLE     0x0Fu
/* flags, high nibble: fixed when written, covered by the CRC */
#define LOG_F_TIME_VALID  0x10u   /* clock was set when the record was taken */
#define LOG_F_TYPE_MASK   0x60u
#define LOG_TYPE_DATA     0x00u   /* a sensor reading */
#define LOG_TYPE_TIMECHG  0x20u   /* clock was changed: time = new, old time in bytes 8-11 */
#define LOG_TYPE_MARKER   0x40u   /* written after a full delete to keep seq running */

typedef struct __attribute__((packed)) {
  uint32_t seq;          /* running number, never reused */
  uint32_t time;         /* Unix seconds (seconds since power-up if not valid) */
  uint16_t moisture_mV;  /* 0xFFFF = not read */
  int16_t  temp_c100;    /* 0.01 degC, 0x7FFF = not read */
  uint16_t vdda_mV;
  uint8_t  flags;
  uint8_t  crc;          /* CRC-8 over bytes 0-13 and the high nibble of flags */
} log_rec_t;

typedef bool (*log_visit_t)(uint32_t slot, const log_rec_t *rec, void *ctx); /* false stops */

bool     Datalog_Init(void);     /* scans the flash; false if the flash does not answer */
bool     Datalog_Append(uint32_t time, bool time_valid, uint16_t moisture_mV, int16_t temp_c100, uint16_t vdda_mV);
bool     Datalog_AppendTimeChange(uint32_t old_time, uint32_t new_time);
/* Up to max unsent records, oldest first. Records older than max_age_s are
 * skipped when now_valid (0 = no age limit). Returns the number found. */
int      Datalog_GetUnsent(log_rec_t *recs, uint32_t *slots, int max, uint32_t now, bool now_valid, uint32_t max_age_s);
bool     Datalog_MarkSent(uint32_t slot);
uint32_t Datalog_CountUnsent(void);
uint32_t Datalog_CountLive(void);
uint32_t Datalog_UsedPermille(void);         /* 0-1000 */
uint32_t Datalog_NextSeq(void);
/* Visits every live record, oldest first. Data records only if
 * from <= time <= to; time-change and marker records always. */
void     Datalog_ReadRange(uint32_t from, uint32_t to, log_visit_t fn, void *ctx);
uint32_t Datalog_DeleteBefore(uint32_t time); /* returns the number deleted */
bool     Datalog_DeleteAll(void);
bool     Datalog_Format(void);      /* erase the ring and restart seq at 1 (first use of the chip) */

bool     Datalog_RecordValid(const log_rec_t *rec);

#endif
