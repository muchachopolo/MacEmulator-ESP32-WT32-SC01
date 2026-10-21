/* Host shim: tasks. mac_emu_run() calls taskYIELD() exactly once at the
 * end of every emulated frame, so the host uses it as its per-frame hook
 * (frame limit, screenshots). The harness runs single-threaded. */
#pragma once
#include "freertos/FreeRTOS.h"
#ifdef __cplusplus
extern "C" {
#endif
void host_frame_hook(void);
void vTaskDelay(TickType_t ticks);
#define taskYIELD()         host_frame_hook()
#define xPortGetCoreID()    1
#ifdef __cplusplus
}
#endif
