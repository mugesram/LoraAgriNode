#ifndef MAIN_H
#define MAIN_H

#include "board.h"

#define BOARD_NAME      "entc-sensornode-1"
#ifdef POWER_TEST
#define FW_VERSION      "1.0.0-test-" PT_NAME
#else
#define FW_VERSION      "1.0.0"
#endif
#define PROTO_VERSION   1

/* written to g_boot_request before a reset to enter the ROM USB bootloader */
#define BOOT_REQUEST_DFU  0xB00710ADu
extern uint32_t g_boot_request;
extern uint32_t g_reset_flags;   /* copy of RCC->CSR taken at start-up */

void Error_Handler(void);

#endif
