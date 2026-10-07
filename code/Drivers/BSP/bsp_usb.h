/*
 * USB serial port (TinyUSB CDC). USB runs only while a PC is connected:
 * HSI48 and the USB peripheral are off otherwise.
 */
#ifndef BSP_USB_H
#define BSP_USB_H

#include <stdbool.h>
#include <stdint.h>

bool Usb_HostPresent(void);   /* a USB host (PC or hub) is on the cable; call only while USB is stopped */
void Usb_Start(void);
void Usb_Stop(void);
bool Usb_IsActive(void);      /* Usb_Start() was called */
bool Usb_IsLinked(void);      /* enumerated by the host and not suspended */
void Usb_Task(void);
int  Usb_ReadChar(void);      /* -1 = nothing */
bool Usb_Write(const char *s, uint32_t len);   /* false = host not reading */

#endif
