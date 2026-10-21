/* Pi shim: esp_timer_get_time -> BCM2836 1 MHz system timer. */
#pragma once
#include <stdint.h>
#include "pi_platform.h"
static inline int64_t esp_timer_get_time(void) { return (int64_t)pi_micros(); }
