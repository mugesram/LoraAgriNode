/* Data log ring in the external flash. See datalog.h for the layout. */
#include <string.h>
#include "app_hooks.h"
#include "bsp_flash.h"
#include "datalog.h"

#define REC_SIZE        16u
#define CHUNK_RECS      32u
#define SLOT_ADDR(slot) (LOG_FIRST_SECTOR * W25Q_SECTOR_SIZE + (slot) * REC_SIZE)
#define UNSENT_BUDGET   2048   /* slots looked at per Datalog_GetUnsent call */

_Static_assert(sizeof(log_rec_t) == REC_SIZE, "record must be 16 bytes");

static uint32_t s_head;      /* slot of the next write */
static uint32_t s_seq_next;
static uint32_t s_live;      /* live data records */
static uint32_t s_unsent;    /* live data records still to send */
static uint32_t s_cursor;    /* where the search for unsent records starts */
static log_rec_t s_chunk[CHUNK_RECS];

/* ----------------------------------------------------------------- helpers */
static uint8_t rec_crc(const log_rec_t *r) {
  const uint8_t *p = (const uint8_t *) r;
  uint8_t crc = 0xA5; /* not 0, so an all-zero record is invalid */
  for (unsigned i = 0; i < 15; i++) {
    uint8_t b = (i == 14) ? (uint8_t) (p[i] & ~LOG_F_MUTABLE) : p[i];
    crc ^= b;
    for (int k = 0; k < 8; k++) crc = (crc & 0x80) ? (uint8_t) ((crc << 1) ^ 0x07) : (uint8_t) (crc << 1);
  }
  return crc;
}

static bool rec_blank(const log_rec_t *r) {
  const uint8_t *p = (const uint8_t *) r;
  for (unsigned i = 0; i < REC_SIZE; i++) if (p[i] != 0xFF) return false;
  return true;
}

bool Datalog_RecordValid(const log_rec_t *r) { return !rec_blank(r) && rec_crc(r) == r->crc; }

static bool is_data(const log_rec_t *r)    { return (r->flags & LOG_F_TYPE_MASK) == LOG_TYPE_DATA; }
static bool is_live(const log_rec_t *r)    { return (r->flags & LOG_F_LIVE) != 0; }
static bool is_unsent(const log_rec_t *r)  { return (r->flags & LOG_F_UNSENT) != 0; }

static uint32_t next_slot(uint32_t slot) { return (slot + 1) % LOG_CAPACITY; }
static uint32_t sector_start(uint32_t sector) { return (sector % LOG_SECTORS) * LOG_RECS_PER_SECTOR; }

static bool read_slot(uint32_t slot, log_rec_t *r) { return Flash_Read(SLOT_ADDR(slot), r, REC_SIZE); }

static bool write_flags(uint32_t slot, uint8_t flags) {
  return Flash_Write(SLOT_ADDR(slot) + 14, &flags, 1);
}

/* Visits the valid records of one sector in order; stops at the first blank
 * slot. Returns false if the visitor asked to stop or the flash failed. */
static bool visit_sector(uint32_t sector, log_visit_t fn, void *ctx) {
  log_rec_t first;
  uint32_t base = sector_start(sector);
  if (!read_slot(base, &first)) return false;
  if (rec_blank(&first)) return true;
  for (uint32_t off = 0; off < LOG_RECS_PER_SECTOR; off += CHUNK_RECS) {
    if (!Flash_Read(SLOT_ADDR(base + off), s_chunk, sizeof s_chunk)) return false;
    for (uint32_t k = 0; k < CHUNK_RECS; k++) {
      if (rec_blank(&s_chunk[k])) return true;
      if (!Datalog_RecordValid(&s_chunk[k])) continue;
      log_rec_t r = s_chunk[k]; /* the visitor may use the flash */
      if (!fn(base + off + k, &r, ctx)) return false;
    }
  }
  return true;
}

/* Visits every valid record, oldest first. */
static void visit_all(log_visit_t fn, void *ctx) {
  uint32_t hs = s_head / LOG_RECS_PER_SECTOR;
  /* a head at the start of a sector has not erased it yet: it is the oldest */
  uint32_t start = (s_head % LOG_RECS_PER_SECTOR == 0) ? hs : (hs + 1) % LOG_SECTORS;
  for (uint32_t i = 0; i < LOG_SECTORS; i++) {
    if (!visit_sector((start + i) % LOG_SECTORS, fn, ctx)) return;
    Board_Idle();
  }
}

static bool sector_is_blank(uint32_t sector) {
  uint32_t base = sector_start(sector);
  for (uint32_t off = 0; off < LOG_RECS_PER_SECTOR; off += CHUNK_RECS) {
    if (!Flash_Read(SLOT_ADDR(base + off), s_chunk, sizeof s_chunk)) return false;
    for (uint32_t k = 0; k < CHUNK_RECS; k++) if (!rec_blank(&s_chunk[k])) return false;
  }
  return true;
}

/* -------------------------------------------------------------------- init */
static bool count_visit(uint32_t slot, const log_rec_t *r, void *ctx) {
  bool *have_cursor = ctx;
  if (is_data(r) && is_live(r)) {
    s_live++;
    if (is_unsent(r)) {
      s_unsent++;
      if (!*have_cursor) { s_cursor = slot; *have_cursor = true; }
    }
  }
  return true;
}

bool Datalog_Init(void) {
  log_rec_t r;
  uint32_t max_seq = 0, hs = 0;
  bool found = false, have_cursor = false;

  s_head = 0; s_seq_next = 1; s_live = 0; s_unsent = 0; s_cursor = 0;

  /* the sector whose first record has the highest seq is being written */
  for (uint32_t sec = 0; sec < LOG_SECTORS; sec++) {
    if (!read_slot(sector_start(sec), &r)) return false;
    if (Datalog_RecordValid(&r) && (!found || r.seq > max_seq)) { max_seq = r.seq; hs = sec; found = true; }
    if ((sec & 31) == 0) Board_Idle();
  }
  if (found) {
    uint32_t used = 0;
    uint32_t base = sector_start(hs);
    for (uint32_t k = 0; k < LOG_RECS_PER_SECTOR; k++) {
      if (!read_slot(base + k, &r)) return false;
      if (rec_blank(&r)) break;
      used = k + 1;
      if (Datalog_RecordValid(&r) && r.seq > max_seq) max_seq = r.seq;
    }
    s_head = (base + used) % LOG_CAPACITY;
    s_seq_next = max_seq + 1;
  }
  visit_all(count_visit, &have_cursor);
  if (!have_cursor) s_cursor = s_head;
  return true;
}

/* ------------------------------------------------------------------ append */
static bool forget_visit(uint32_t slot, const log_rec_t *r, void *ctx) {
  (void) slot; (void) ctx;
  if (is_data(r) && is_live(r)) {
    if (s_live) s_live--;
    if (is_unsent(r) && s_unsent) s_unsent--;
  }
  return true;
}

/* The write position enters a sector: drop its old records and erase it. */
static bool enter_sector(uint32_t sector) {
  if (sector_is_blank(sector)) return true;
  visit_sector(sector, forget_visit, NULL);
  if (s_cursor / LOG_RECS_PER_SECTOR == sector) s_cursor = sector_start(sector + 1);
  return Flash_EraseSector(SLOT_ADDR(sector_start(sector)));
}

static bool write_rec(log_rec_t *r) {
  bool ok;
  if (s_head % LOG_RECS_PER_SECTOR == 0 && !enter_sector(s_head / LOG_RECS_PER_SECTOR)) return false;
  if (s_unsent == 0) s_cursor = s_head;
  r->seq = s_seq_next;
  r->crc = rec_crc(r);
  ok = Flash_Write(SLOT_ADDR(s_head), r, REC_SIZE);
  /* advance also after a failure, so that a bad slot is skipped */
  s_head = next_slot(s_head);
  s_seq_next++;
  return ok;
}

bool Datalog_Append(uint32_t time, bool time_valid, uint16_t moisture_mV, int16_t temp_c100, uint16_t vdda_mV) {
  log_rec_t r;
  r.time = time;
  r.moisture_mV = moisture_mV;
  r.temp_c100 = temp_c100;
  r.vdda_mV = vdda_mV;
  r.flags = (uint8_t) (LOG_TYPE_DATA | (time_valid ? LOG_F_TIME_VALID : 0) | LOG_F_MUTABLE);
  if (!write_rec(&r)) return false;
  s_live++;
  s_unsent++;
  return true;
}

static bool append_special(uint8_t type, uint32_t time, uint32_t payload) {
  log_rec_t r;
  r.time = time;
  memcpy(&r.moisture_mV, &payload, 4); /* bytes 8-11 */
  r.vdda_mV = 0;
  /* never sent over LoRa: written with the unsent bit already cleared */
  r.flags = (uint8_t) (type | LOG_F_TIME_VALID | (LOG_F_MUTABLE & ~LOG_F_UNSENT));
  return write_rec(&r);
}

bool Datalog_AppendTimeChange(uint32_t old_time, uint32_t new_time) {
  return append_special(LOG_TYPE_TIMECHG, new_time, old_time);
}

/* ------------------------------------------------------------------ unsent */
int Datalog_GetUnsent(log_rec_t *recs, uint32_t *slots, int max, uint32_t now, bool now_valid, uint32_t max_age_s) {
  log_rec_t r;
  uint32_t c = s_cursor;
  uint32_t left = (s_head + LOG_CAPACITY - s_cursor) % LOG_CAPACITY;
  int n = 0, budget = UNSENT_BUDGET;
  bool nothing_pending_yet = true;

  if (s_unsent == 0 || max <= 0) return 0;
  if (left == 0) left = LOG_CAPACITY; /* ring completely full of unsent records */

  while (left && n < max && budget-- > 0) {
    if (!read_slot(c, &r)) break;
    if (rec_blank(&r)) {
      /* rest of this sector is empty: continue with the next one */
      uint32_t nxt = sector_start(c / LOG_RECS_PER_SECTOR + 1);
      uint32_t skip = (nxt + LOG_CAPACITY - c) % LOG_CAPACITY;
      if (skip == 0 || skip > left) skip = left;
      left -= skip;
      c = (c + skip) % LOG_CAPACITY;
      if (nothing_pending_yet) s_cursor = c;
      continue;
    }
    bool pending = Datalog_RecordValid(&r) && is_data(&r) && is_live(&r) && is_unsent(&r);
    if (pending && max_age_s && now_valid && (r.flags & LOG_F_TIME_VALID)
        && now > r.time && now - r.time > max_age_s) {
      pending = false; /* too old: stays in the flash, but is not sent */
      if (s_unsent) s_unsent--;
    }
    c = next_slot(c);
    left--;
    if (pending) {
      recs[n] = r;
      slots[n] = (c + LOG_CAPACITY - 1) % LOG_CAPACITY;
      n++;
      nothing_pending_yet = false;
    } else if (nothing_pending_yet) {
      s_cursor = c;
    }
    if ((budget & 63) == 0) Board_Idle();
  }
  if (left == 0 && n == 0) s_unsent = 0; /* walked to the head and found nothing */
  return n;
}

bool Datalog_MarkSent(uint32_t slot) {
  log_rec_t r;
  if (slot >= LOG_CAPACITY || !read_slot(slot, &r)) return false;
  if (!Datalog_RecordValid(&r) || !is_unsent(&r)) return true;
  if (!write_flags(slot, (uint8_t) (r.flags & ~LOG_F_UNSENT))) return false;
  if (is_data(&r) && is_live(&r) && s_unsent) s_unsent--;
  return true;
}

uint32_t Datalog_CountUnsent(void) { return s_unsent; }
uint32_t Datalog_CountLive(void) { return s_live; }
uint32_t Datalog_NextSeq(void) { return s_seq_next; }

uint32_t Datalog_UsedPermille(void) {
  uint32_t p = (uint32_t) (((uint64_t) s_live * 1000u + LOG_CAPACITY - 1) / LOG_CAPACITY);
  return p > 1000 ? 1000 : p;
}

/* -------------------------------------------------------------------- read */
typedef struct { uint32_t from, to; log_visit_t fn; void *ctx; } range_ctx_t;

static bool range_visit(uint32_t slot, const log_rec_t *r, void *ctx) {
  range_ctx_t *rc = ctx;
  if (!is_live(r)) return true;
  if (is_data(r) && (r->time < rc->from || r->time > rc->to)) return true;
  return rc->fn(slot, r, rc->ctx);
}

void Datalog_ReadRange(uint32_t from, uint32_t to, log_visit_t fn, void *ctx) {
  range_ctx_t rc = { from, to, fn, ctx };
  visit_all(range_visit, &rc);
}

/* ------------------------------------------------------------------ delete */
typedef struct { uint32_t before; uint32_t count; } delete_ctx_t;

static bool delete_visit(uint32_t slot, const log_rec_t *r, void *ctx) {
  delete_ctx_t *dc = ctx;
  if (!is_data(r) || !is_live(r)) return true;
  if (!(r->flags & LOG_F_TIME_VALID) || r->time >= dc->before) return true;
  if (!write_flags(slot, (uint8_t) (r->flags & ~(LOG_F_LIVE | LOG_F_UNSENT)))) return true;
  if (s_live) s_live--;
  if (is_unsent(r) && s_unsent) s_unsent--;
  dc->count++;
  return true;
}

uint32_t Datalog_DeleteBefore(uint32_t time) {
  delete_ctx_t dc = { time, 0 };
  visit_all(delete_visit, &dc);
  return dc.count;
}

static bool erase_ring(void) {
  bool ok = true;
  for (uint32_t sec = 0; sec < LOG_SECTORS; sec++) {
    if (!sector_is_blank(sec)) ok = Flash_EraseSector(SLOT_ADDR(sector_start(sec))) && ok;
    Board_Idle();
  }
  s_head = 0; s_cursor = 0; s_live = 0; s_unsent = 0;
  return ok;
}

bool Datalog_DeleteAll(void) {
  bool ok = erase_ring();
  /* a marker record keeps the sequence number running after the erase */
  return append_special(LOG_TYPE_MARKER, 0, 0) && ok;
}

/* First use of a flash chip that holds something else: start from nothing. */
bool Datalog_Format(void) {
  bool ok = erase_ring();
  s_seq_next = 1;
  return ok;
}
