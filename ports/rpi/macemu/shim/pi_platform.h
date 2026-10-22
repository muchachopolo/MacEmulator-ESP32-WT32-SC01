/* Services the Circle side (platform.cpp) provides to the C emulator core. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint64_t pi_micros(void);
void pi_delay_ms(unsigned ms);
void pi_log(int level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void pi_restart(void);
void pi_frame_hook(void);
#ifdef __cplusplus
}
#endif
