/* Host shim: CPU cycle counter. ESP32 returns a 32-bit 240MHz CCOUNT;
 * we return a wrapping 32-bit count at a nominal 240MHz derived from
 * CLOCK_MONOTONIC so the firmware's profiling math stays meaningful. */
#pragma once
#include <stdint.h>
#include <time.h>
typedef uint32_t esp_cpu_cycle_count_t;
static inline esp_cpu_cycle_count_t esp_cpu_get_cycle_count(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
    return (uint32_t)(ns * 240 / 1000);
}
