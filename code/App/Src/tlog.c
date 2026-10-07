/* Timing log in the MCU flash. See tlog.h. */
#include <string.h>
#include "board.h"
#include "app_hooks.h"
#include "timekeep.h"
#include "tlog.h"

#define TLOG_BASE  0x08010000u   /* the firmware ends well below this */
#define TLOG_END   0x08040000u
#define PAGE_SIZE  2048u
#define CAPACITY   ((TLOG_END - TLOG_BASE) / sizeof(tlog_rec_t))

static uint32_t s_count;

const tlog_rec_t *Tlog_Get(uint32_t i) { return (const tlog_rec_t *) (TLOG_BASE + i * sizeof(tlog_rec_t)); }
uint32_t Tlog_Count(void) { return s_count; }
uint32_t Tlog_Capacity(void) { return CAPACITY; }

void Tlog_Init(void) {
  s_count = 0;
  while (s_count < CAPACITY && Tlog_Get(s_count)->type != 0xFF) s_count++;
}

bool Tlog_Append(uint8_t type, uint8_t flags, uint16_t seq, const uint32_t *v) {
  tlog_rec_t r;
  uint64_t dw;
  bool ok = true;
  if (s_count >= CAPACITY) return false;
  r.type = type;
  r.flags = flags;
  r.seq = seq;
  r.time = Time_Now();
  memcpy(r.v, v, sizeof r.v);

  uint32_t addr = TLOG_BASE + s_count * sizeof r;
  HAL_FLASH_Unlock();
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
  for (uint32_t i = 0; i < sizeof r && ok; i += 8) {
    memcpy(&dw, (const uint8_t *) &r + i, 8);
    ok = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr + i, dw) == HAL_OK;
  }
  HAL_FLASH_Lock();
  s_count++; /* a record that failed half-way is skipped, not written over */
  return ok;
}

bool Tlog_Clear(void) {
  FLASH_EraseInitTypeDef e = { .TypeErase = FLASH_TYPEERASE_PAGES, .Banks = FLASH_BANK_1, .NbPages = 1 };
  uint32_t fault;
  bool ok = true;
  HAL_FLASH_Unlock();
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
  for (uint32_t a = TLOG_BASE; a < TLOG_END && ok; a += PAGE_SIZE) {
    const uint32_t *w = (const uint32_t *) a;
    bool blank = true;
    for (uint32_t i = 0; i < PAGE_SIZE / 4 && blank; i++) blank = w[i] == 0xFFFFFFFFu;
    if (blank) continue;
    e.Page = (a - 0x08000000u) / PAGE_SIZE;
    ok = HAL_FLASHEx_Erase(&e, &fault) == HAL_OK;
    Board_Idle();
  }
  HAL_FLASH_Lock();
  Tlog_Init();
  return ok;
}
