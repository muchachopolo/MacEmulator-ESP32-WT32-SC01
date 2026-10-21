/* Host shim: esp_timer_get_time -> CLOCK_MONOTONIC microseconds, or the
 * virtual clock frames * --vclock US when that option is given. */
#pragma once
#include <stdint.h>
#include <time.h>
extern int64_t host_vclock_us;   /* host_main.c: >= 0 with --vclock */
static inline int64_t esp_timer_get_time(void)
{
    if (host_vclock_us >= 0) return host_vclock_us;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
