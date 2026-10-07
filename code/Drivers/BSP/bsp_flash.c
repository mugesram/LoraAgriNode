/* W25Q80DV driver on SPI1 (HAL). */
#include <string.h>
#include "board.h"
#include "app_hooks.h"
#include "bsp_flash.h"

#define CMD_WRITE_ENABLE   0x06
#define CMD_READ_STATUS    0x05
#define CMD_READ_DATA      0x03
#define CMD_PAGE_PROGRAM   0x02
#define CMD_SECTOR_ERASE   0x20
#define CMD_POWER_DOWN     0xB9
#define CMD_RELEASE_PD     0xAB
#define CMD_JEDEC_ID       0x9F
#define STATUS_BUSY        0x01

#define SPI_TIMEOUT_MS     100
#define PROGRAM_TIMEOUT_MS 10    /* tPP max 3 ms */
#define ERASE_TIMEOUT_MS   500   /* tSE max 300 ms */

static bool s_awake;

static void cs_low(void)  { HAL_GPIO_WritePin(FLASH_CS_PORT, FLASH_CS_PIN, GPIO_PIN_RESET); }
static void cs_high(void) { HAL_GPIO_WritePin(FLASH_CS_PORT, FLASH_CS_PIN, GPIO_PIN_SET); }

static bool spi_tx(const uint8_t *buf, uint32_t len) {
  return HAL_SPI_Transmit(&hspi1, (uint8_t *) buf, (uint16_t) len, SPI_TIMEOUT_MS) == HAL_OK;
}

static bool spi_rx(uint8_t *buf, uint32_t len) {
  return HAL_SPI_Receive(&hspi1, buf, (uint16_t) len, SPI_TIMEOUT_MS) == HAL_OK;
}

static void cmd_only(uint8_t cmd) {
  cs_low();
  spi_tx(&cmd, 1);
  cs_high();
}

static bool cmd_addr(uint8_t cmd, uint32_t addr) {
  uint8_t h[4] = { cmd, (uint8_t) (addr >> 16), (uint8_t) (addr >> 8), (uint8_t) addr };
  return spi_tx(h, 4);
}

void Flash_Wake(void) {
  if (s_awake) return;
  cmd_only(CMD_RELEASE_PD);
  Board_DelayUs(30); /* tRES1 = 3 us */
  s_awake = true;
}

void Flash_Sleep(void) {
  if (!s_awake) return;
  Flash_WaitReady(ERASE_TIMEOUT_MS);
  cmd_only(CMD_POWER_DOWN);
  s_awake = false;
}

void Flash_ReadId(uint8_t id[3]) {
  uint8_t cmd = CMD_JEDEC_ID;
  Flash_Wake();
  cs_low();
  spi_tx(&cmd, 1);
  spi_rx(id, 3);
  cs_high();
}

bool Flash_Init(void) {
  uint8_t id[3] = {0};
  /* the chip may be in power-down from before a reset: always send the wake command */
  s_awake = false;
  Flash_Wake();
  Flash_ReadId(id);
  return id[0] == 0xEF && id[1] == 0x40 && id[2] == 0x14;
}

bool Flash_WaitReady(uint32_t timeout_ms) {
  uint8_t cmd = CMD_READ_STATUS, st = STATUS_BUSY;
  uint32_t t0 = HAL_GetTick();
  for (;;) {
    cs_low();
    spi_tx(&cmd, 1);
    spi_rx(&st, 1);
    cs_high();
    if (!(st & STATUS_BUSY)) return true;
    if (HAL_GetTick() - t0 > timeout_ms) return false;
    Board_Idle();
  }
}

bool Flash_Read(uint32_t addr, void *buf, uint32_t len) {
  bool ok;
  if (addr + len > W25Q_SIZE_BYTES) return false;
  Flash_Wake();
  cs_low();
  ok = cmd_addr(CMD_READ_DATA, addr) && spi_rx(buf, len);
  cs_high();
  return ok;
}

static bool program_page(uint32_t addr, const uint8_t *buf, uint32_t len) {
  bool ok;
  cmd_only(CMD_WRITE_ENABLE);
  cs_low();
  ok = cmd_addr(CMD_PAGE_PROGRAM, addr) && spi_tx(buf, len);
  cs_high();
  return ok && Flash_WaitReady(PROGRAM_TIMEOUT_MS);
}

bool Flash_Write(uint32_t addr, const void *buf, uint32_t len) {
  const uint8_t *p = buf;
  uint8_t check[32];
  uint32_t a = addr, left = len;

  if (addr + len > W25Q_SIZE_BYTES) return false;
  Flash_Wake();
  while (left) {
    uint32_t n = W25Q_PAGE_SIZE - (a % W25Q_PAGE_SIZE);
    if (n > left) n = left;
    if (!program_page(a, p, n)) return false;
    a += n; p += n; left -= n;
  }
  /* read back */
  p = buf; a = addr; left = len;
  while (left) {
    uint32_t n = left > sizeof check ? sizeof check : left;
    if (!Flash_Read(a, check, n) || memcmp(check, p, n) != 0) return false;
    a += n; p += n; left -= n;
  }
  return true;
}

bool Flash_EraseSector(uint32_t addr) {
  bool ok;
  if (addr >= W25Q_SIZE_BYTES) return false;
  Flash_Wake();
  cmd_only(CMD_WRITE_ENABLE);
  cs_low();
  ok = cmd_addr(CMD_SECTOR_ERASE, addr & ~(W25Q_SECTOR_SIZE - 1));
  cs_high();
  return ok && Flash_WaitReady(ERASE_TIMEOUT_MS);
}
