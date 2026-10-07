/*
 * PC test of the data log and settings code against a simulated flash.
 * The simulated flash behaves like NOR: writes can only clear bits, an
 * erase sets a whole 4 KB sector to 0xFF. Run with `make test`.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bsp_flash.h"
#include "config.h"
#include "datalog.h"

static uint8_t mem[W25Q_SIZE_BYTES];
static long writes_left = -1; /* >= 0: simulated power cut after this many writes */
static unsigned erases;

void Board_Idle(void) {}
bool Flash_Init(void) { return true; }
void Flash_Wake(void) {}
void Flash_Sleep(void) {}
bool Flash_WaitReady(uint32_t t) { (void) t; return true; }
bool Flash_Read(uint32_t addr, void *buf, uint32_t len) {
  assert(addr + len <= W25Q_SIZE_BYTES);
  memcpy(buf, mem + addr, len);
  return true;
}
bool Flash_Write(uint32_t addr, const void *buf, uint32_t len) {
  const uint8_t *p = buf;
  assert(addr + len <= W25Q_SIZE_BYTES);
  for (uint32_t i = 0; i < len; i++) {
    if (writes_left == 0) return false; /* power lost in the middle of a write */
    mem[addr + i] &= p[i];
  }
  if (writes_left > 0) writes_left--;
  return memcmp(mem + addr, buf, len) == 0;
}
bool Flash_EraseSector(uint32_t addr) {
  memset(mem + (addr & ~(W25Q_SECTOR_SIZE - 1)), 0xFF, W25Q_SECTOR_SIZE);
  erases++;
  return true;
}

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned visited;
static uint32_t first_seq, last_seq;
static bool count_cb(uint32_t slot, const log_rec_t *r, void *ctx) {
  (void) slot; (void) ctx;
  if ((r->flags & LOG_F_TYPE_MASK) != LOG_TYPE_DATA) return true;
  if (!visited) first_seq = r->seq; else CHECK(r->seq > last_seq); /* oldest first */
  last_seq = r->seq;
  visited++;
  return true;
}
static unsigned count_all(void) { visited = 0; Datalog_ReadRange(0, 0xFFFFFFFFu, count_cb, NULL); return visited; }

static void add(uint32_t t) { CHECK(Datalog_Append(t, true, 1000, 2500, 3300)); }

int main(void) {
  log_rec_t recs[15];
  uint32_t slots[15];
  int n;

  memset(mem, 0xFF, sizeof mem);

  /* ---- settings */
  CHECK(!Config_Load());                 /* empty flash -> defaults */
  CHECK(g_cfg.period_s == 60 && g_cfg.send_enable == 1);
  CHECK(Config_Set("period_s", 300) && !Config_Set("period_s", 1) && !Config_Set("nope", 1));
  CHECK(Config_Save());
  CHECK(Config_Set("tx_dbm", 14) && Config_Save());
  Config_Defaults();
  CHECK(Config_Load() && g_cfg.period_s == 300 && g_cfg.tx_dbm == 14 && g_cfg.version == 2);
  mem[W25Q_SECTOR_SIZE + 9] ^= 0x01;    /* damage the newer copy (sector 1) */
  CHECK(Config_Load() && g_cfg.period_s == 300 && g_cfg.tx_dbm == 10 && g_cfg.version == 1);
  CHECK(Config_Save() && g_cfg.version == 2);
  printf("settings: OK\n");

  /* ---- empty log */
  CHECK(Datalog_Init());
  CHECK(Datalog_CountLive() == 0 && Datalog_CountUnsent() == 0 && Datalog_NextSeq() == 1);
  CHECK(Datalog_GetUnsent(recs, slots, 15, 0, false, 0) == 0);

  /* ---- append, send, restart */
  for (uint32_t i = 0; i < 10; i++) add(1000 + i);
  CHECK(Datalog_CountLive() == 10 && Datalog_CountUnsent() == 10);
  n = Datalog_GetUnsent(recs, slots, 4, 2000, true, 0);
  CHECK(n == 4 && recs[0].seq == 1 && recs[3].seq == 4);
  for (int i = 0; i < n; i++) CHECK(Datalog_MarkSent(slots[i]));
  CHECK(Datalog_CountUnsent() == 6);
  CHECK(Datalog_Init());                 /* as after a reset */
  CHECK(Datalog_CountLive() == 10 && Datalog_CountUnsent() == 6 && Datalog_NextSeq() == 11);
  n = Datalog_GetUnsent(recs, slots, 15, 2000, true, 0);
  CHECK(n == 6 && recs[0].seq == 5 && recs[5].seq == 10);
  printf("append / send / restart: OK\n");

  /* ---- 3-day rule: old records are skipped, not deleted */
  n = Datalog_GetUnsent(recs, slots, 15, 1007 + 100, true, 100);
  CHECK(n == 3 && recs[0].seq == 8);     /* times 1007 to 1009 are within 100 s */
  CHECK(Datalog_CountLive() == 10 && count_all() == 10);
  CHECK(Datalog_Init() && Datalog_CountUnsent() == 6);
  printf("age limit: OK\n");

  /* ---- time-change record is stored but never sent */
  CHECK(Datalog_AppendTimeChange(5, 5000));
  CHECK(Datalog_CountUnsent() == 6 && Datalog_NextSeq() == 12);

  /* ---- delete by date */
  CHECK(Datalog_DeleteBefore(1005) == 5);
  CHECK(Datalog_CountLive() == 5 && count_all() == 5 && first_seq == 6);
  CHECK(Datalog_CountUnsent() == 5);     /* seq 5 was unsent and is gone */
  CHECK(Datalog_Init() && Datalog_CountLive() == 5 && Datalog_CountUnsent() == 5);
  printf("delete by date: OK\n");

  /* ---- ring wrap: fill far beyond capacity */
  for (uint32_t i = 0; i < LOG_CAPACITY * 3 + 77; i++) add(10000 + i);
  CHECK(Datalog_CountLive() <= LOG_CAPACITY && Datalog_CountLive() > LOG_CAPACITY - LOG_RECS_PER_SECTOR);
  CHECK(count_all() == Datalog_CountLive());
  CHECK(Datalog_CountUnsent() == Datalog_CountLive());
  uint32_t live = Datalog_CountLive(), seq = Datalog_NextSeq();
  CHECK(Datalog_Init() && Datalog_CountLive() == live && Datalog_NextSeq() == seq);
  CHECK(last_seq == seq - 1);
  /* send everything in packets; order must be oldest first with no gaps */
  uint32_t expect = first_seq, sent = 0;
  while ((n = Datalog_GetUnsent(recs, slots, 15, 0, false, 0)) > 0) {
    for (int i = 0; i < n; i++) { CHECK(recs[i].seq == expect++); CHECK(Datalog_MarkSent(slots[i])); sent++; }
  }
  CHECK(sent == live && Datalog_CountUnsent() == 0);
  add(99999);
  n = Datalog_GetUnsent(recs, slots, 15, 0, false, 0);
  CHECK(n == 1 && recs[0].seq == seq);
  printf("ring wrap (%u records, %u erases): OK\n", (unsigned) live, erases);

  /* ---- power cut during a record write */
  CHECK(Datalog_Init());
  live = Datalog_CountLive(); seq = Datalog_NextSeq();
  writes_left = 0;
  CHECK(!Datalog_Append(1, true, 1, 1, 1));
  writes_left = -1;
  CHECK(Datalog_Init() && Datalog_CountLive() == live);
  add(123456);
  CHECK(Datalog_CountLive() == live + 1 && count_all() == live + 1);
  CHECK(Datalog_Init() && Datalog_CountLive() == live + 1 && Datalog_NextSeq() > seq);
  printf("power cut: OK\n");

  /* ---- full delete keeps the sequence number running */
  seq = Datalog_NextSeq();
  CHECK(Datalog_DeleteAll());
  CHECK(Datalog_CountLive() == 0 && Datalog_CountUnsent() == 0 && count_all() == 0);
  CHECK(Datalog_Init() && Datalog_NextSeq() == seq + 1 && Datalog_CountLive() == 0);
  add(5); 
  CHECK(Datalog_GetUnsent(recs, slots, 15, 0, false, 0) == 1 && recs[0].seq == seq + 1);
  CHECK(Config_Load() && g_cfg.period_s == 300); /* settings untouched by the erase */
  printf("full delete: OK\n");

  /* ---- first use of a chip full of old test data */
  srand(1);
  for (uint32_t i = 2 * W25Q_SECTOR_SIZE; i < sizeof mem; i++) mem[i] = (uint8_t) rand();
  CHECK(Datalog_Format());
  CHECK(Datalog_Init() && Datalog_CountLive() == 0 && Datalog_NextSeq() == 1 && count_all() == 0);
  add(7);
  CHECK(Datalog_Init() && Datalog_CountLive() == 1 && Datalog_NextSeq() == 2);
  CHECK(Config_Load() && g_cfg.period_s == 300);
  printf("format: OK\n");

  printf("ALL TESTS PASSED\n");
  return 0;
}
