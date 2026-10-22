/* Pi shim: tasks. mac_emu_run() calls taskYIELD() once at the end of every
 * emulated frame; the single-core bring-up uses it as the per-frame hook
 * (LCD refresh). */
#pragma once
#include "freertos/FreeRTOS.h"
#include "pi_platform.h"
static inline void vTaskDelay(TickType_t ticks) { pi_delay_ms(ticks); }
#define taskYIELD()         pi_frame_hook()
#define xPortGetCoreID()    0
