/*
 * SX1262 (Seeed Wio-SX1262 module) on SPI1, using the Semtech sx126x driver.
 * Radio settings must match the gateway.
 */
#ifndef BSP_RADIO_H
#define BSP_RADIO_H

#include <stdbool.h>
#include <stdint.h>

#define RADIO_FREQ_HZ     866000000u  /* India 865-867 MHz band */
#define RADIO_SYNCWORD    0x12        /* private network */
#define RADIO_PREAMBLE    8
#define RADIO_MAX_PAYLOAD 255
/* SF7, BW 125 kHz, CR 4/5, explicit header, CRC on, standard IQ */

/* error codes returned by Radio_Send / Radio_Receive */
#define RADIO_ERR_SPI      (-1)  /* SPI or BUSY error */
#define RADIO_ERR_NO_DIO1  (-2)  /* radio reports done but DIO1 never rose */
#define RADIO_ERR_NO_DONE  (-3)  /* nothing happened */
#define RADIO_ERR_TIMEOUT  (-4)  /* radio reported a TX time-out */
#define RADIO_ERR_CRC      (-5)  /* packet received with CRC or header error */

/* microsecond timing of the last send and the last receive */
typedef struct {
  uint32_t tx_setup_us;   /* Radio_Send called -> transmitter started */
  uint32_t tx_air_us;     /* transmitter started -> TX done */
  uint32_t rx_setup_us;   /* Radio_Receive called -> receiver started */
  uint32_t rx_wait_us;    /* receiver started -> packet received or time-out */
} radio_timing_t;
extern radio_timing_t g_radio_tm;

int  Radio_Init(void);            /* cold start; 0 on success, else the failed step */
bool Radio_Wake(void);            /* from sleep, or cold start if needed */
void Radio_Sleep(void);           /* sleep with configuration kept */
void Radio_SetTxPower(int8_t dbm);
int  Radio_Send(const uint8_t *buf, uint8_t len, uint32_t timeout_ms);  /* time on air in ms, or error */
int  Radio_Receive(uint8_t *buf, uint8_t max, uint32_t window_ms, int *rssi, int *snr); /* length, 0 = nothing */

#endif
