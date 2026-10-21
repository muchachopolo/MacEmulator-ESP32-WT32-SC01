/* Host shim: heap_caps_* -> libc malloc. */
#pragma once
#include <stdlib.h>
#include <stdint.h>
#define MALLOC_CAP_EXEC      (1 << 0)
#define MALLOC_CAP_32BIT     (1 << 1)
#define MALLOC_CAP_8BIT      (1 << 2)
#define MALLOC_CAP_DMA       (1 << 3)
#define MALLOC_CAP_SPIRAM    (1 << 10)
#define MALLOC_CAP_INTERNAL  (1 << 11)
#define MALLOC_CAP_DEFAULT   (1 << 12)
static inline void *heap_caps_malloc(size_t n, uint32_t caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t c, size_t n, uint32_t caps) { (void)caps; return calloc(c, n); }
static inline void  heap_caps_free(void *p) { free(p); }
/* Pretend we have the board's ~4MB PSRAM free. */
static inline size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 4u * 1024 * 1024; }
