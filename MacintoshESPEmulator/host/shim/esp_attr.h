/* Host shim: ESP-IDF placement attributes are no-ops on the PC. */
#pragma once
#define IRAM_ATTR
#define DRAM_ATTR
#define EXT_RAM_BSS_ATTR
#define EXT_RAM_NOINIT_ATTR
#define RTC_DATA_ATTR
#define RTC_NOINIT_ATTR
#define NOINLINE_ATTR __attribute__((noinline))
#define FORCE_INLINE_ATTR static inline __attribute__((always_inline))
