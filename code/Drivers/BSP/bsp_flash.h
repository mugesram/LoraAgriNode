/*
 * W25Q80DV SPI flash (8 Mbit, 4 KB sectors, 256-byte pages).
 * Every access wakes the chip if needed; Flash_Sleep() puts it in power-down.
 */
#ifndef BSP_FLASH_H
#define BSP_FLASH_H

#include <stdbool.h>
#include <stdint.h>

#define W25Q_SECTOR_SIZE   4096u
#define W25Q_PAGE_SIZE     256u
#define W25Q_SIZE_BYTES    (1024u * 1024u)

bool Flash_Init(void);                 /* true if the JEDEC ID is EF 40 14 */
void Flash_Wake(void);
void Flash_Sleep(void);
bool Flash_Read(uint32_t addr, void *buf, uint32_t len);
bool Flash_Write(uint32_t addr, const void *buf, uint32_t len); /* with read-back check */
bool Flash_EraseSector(uint32_t addr);
bool Flash_WaitReady(uint32_t timeout_ms);
void Flash_ReadId(uint8_t id[3]);

#endif
