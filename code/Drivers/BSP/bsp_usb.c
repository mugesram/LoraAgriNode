/* USB start/stop and the serial port on top of TinyUSB. */
#include "board.h"
#include "app_hooks.h"
#include "bsp_usb.h"
#include "tusb.h"

static bool s_active;

static void usb_pins(uint32_t mode) {
  GPIO_InitTypeDef g = {0};
  g.Pin = USB_DM_PIN | USB_DP_PIN;
  g.Mode = mode;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  g.Alternate = GPIO_AF10_USB_FS;
  HAL_GPIO_Init(USB_PORT, &g);
}

/* The board has no VBUS sense line to the MCU. A USB host has a 15k
 * pull-down on D+, an open connector or a plain charger has not: drive D+
 * high for a moment, release it, and see whether something pulls it low. */
bool Usb_HostPresent(void) {
  GPIO_InitTypeDef g = {0};
  GPIO_PinState level;

  HAL_GPIO_WritePin(USB_PORT, USB_DP_PIN, GPIO_PIN_SET);
  g.Pin = USB_DP_PIN;
  g.Mode = GPIO_MODE_OUTPUT_PP;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(USB_PORT, &g);
  Board_DelayUs(5);
  g.Mode = GPIO_MODE_INPUT;
  HAL_GPIO_Init(USB_PORT, &g);
  Board_DelayUs(100);
  level = HAL_GPIO_ReadPin(USB_PORT, USB_DP_PIN);
  g.Mode = GPIO_MODE_ANALOG;
  HAL_GPIO_Init(USB_PORT, &g);
  return level == GPIO_PIN_RESET;
}

void Usb_Start(void) {
  RCC_OscInitTypeDef osc = {0};
  tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO };
  if (s_active) return;

  /* 48 MHz for USB from HSI48, trimmed by the CRS from the USB start-of-frame */
  osc.OscillatorType = RCC_OSCILLATORTYPE_HSI48;
  osc.HSI48State = RCC_HSI48_ON;
  osc.PLL.PLLState = RCC_PLL_NONE;
  HAL_RCC_OscConfig(&osc);
  __HAL_RCC_USB_CONFIG(RCC_USBCLKSOURCE_HSI48);
  __HAL_RCC_CRS_CLK_ENABLE();
  CRS->CFGR = (CRS->CFGR & ~CRS_CFGR_SYNCSRC) | CRS_CFGR_SYNCSRC_1; /* USB SOF */
  CRS->CR |= CRS_CR_AUTOTRIMEN | CRS_CR_CEN;

  HAL_PWREx_EnableVddUSB();
  __HAL_RCC_USB_CLK_ENABLE();
  usb_pins(GPIO_MODE_AF_PP);

  tusb_init(0, &dev_init);
  s_active = true;
}

void Usb_Stop(void) {
  RCC_OscInitTypeDef osc = {0};
  if (!s_active) return;
  s_active = false;

  tud_disconnect();
  HAL_Delay(5);
  tusb_deinit(0);
  USB->CNTR = USB_CNTR_FRES | USB_CNTR_PDWN; /* transceiver powered down */
  __HAL_RCC_USB_CLK_DISABLE();
  HAL_PWREx_DisableVddUSB();
  usb_pins(GPIO_MODE_ANALOG);

  CRS->CR &= ~(CRS_CR_AUTOTRIMEN | CRS_CR_CEN);
  __HAL_RCC_CRS_CLK_DISABLE();
  osc.OscillatorType = RCC_OSCILLATORTYPE_HSI48;
  osc.HSI48State = RCC_HSI48_OFF;
  osc.PLL.PLLState = RCC_PLL_NONE;
  HAL_RCC_OscConfig(&osc);
}

bool Usb_IsActive(void) { return s_active; }
bool Usb_IsLinked(void) { return s_active && tud_mounted() && !tud_suspended(); }
void Usb_Task(void) { if (s_active) tud_task(); }

int Usb_ReadChar(void) {
  if (!s_active || !tud_cdc_available()) return -1;
  return tud_cdc_read_char();
}

bool Usb_Write(const char *s, uint32_t len) {
  uint32_t t0 = HAL_GetTick();
  if (!s_active) return false;
  while (len) {
    if (!tud_cdc_connected()) return false;
    uint32_t n = tud_cdc_write_available();
    if (n == 0) {
      Board_Idle();
      tud_cdc_write_flush();
      if (HAL_GetTick() - t0 > 1000) return false;
      continue;
    }
    if (n > len) n = len;
    tud_cdc_write(s, n);
    s += n;
    len -= n;
    t0 = HAL_GetTick();
  }
  tud_cdc_write_flush();
  return true;
}

/* time source for TinyUSB */
uint32_t tusb_time_millis_api(void) { return HAL_GetTick(); }
