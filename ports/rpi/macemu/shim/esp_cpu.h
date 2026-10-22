/* Pi shim: the ESP32's 240 MHz CCOUNT, derived from the 1 MHz system timer so
 * the emulator's cycle profiling stays in ESP32 units. */
#pragma once
#include <stdint.h>
#include "pi_platform.h"
typedef uint32_t esp_cpu_cycle_count_t;
static inline esp_cpu_cycle_count_t esp_cpu_get_cycle_count(void)
{
    return (uint32_t)(pi_micros() * 240u);
}
