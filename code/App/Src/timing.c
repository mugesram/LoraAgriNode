/* Stage timers on the DWT cycle counter (wraps after 268 s at 16 MHz). */
#include <string.h>
#include "board.h"
#include "timing.h"

static uint32_t s_start[TM_COUNT];
static uint32_t s_us[TM_COUNT];
static const char *const names[TM_COUNT] = {
  "flash_wake", "radio_wake", "lora_send", "lora_tx", "lora_ack",
  "sensor_wait", "sensor_read", "flash_save", "cycle",
};

void Timing_Start(timing_stage_t s) { s_start[s] = DWT->CYCCNT; }
void Timing_Stop(timing_stage_t s) { s_us[s] = (DWT->CYCCNT - s_start[s]) / (SystemCoreClock / 1000000u); }
void Timing_Add(timing_stage_t s, uint32_t us) { s_us[s] += us; }
void Timing_Clear(void) { memset(s_us, 0, sizeof s_us); }
uint32_t Timing_Us(timing_stage_t s) { return s_us[s]; }
const char *Timing_Name(timing_stage_t s) { return names[s]; }
