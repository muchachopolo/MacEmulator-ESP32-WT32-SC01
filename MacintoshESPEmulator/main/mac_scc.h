#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SCC (Zilog 8530) Stub for Macintosh
 *
 * The Mac uses the SCC for serial ports (modem + printer) and mouse input.
 * For basic emulation, we return safe default values.
 *
 * SCC is at:
 *   Read:  0x800000 - 0x9FFFFF (active on odd bytes)
 *   Write: 0xA00000 - 0xBFFFFF (active on odd bytes)
 */

void scc_init(void);
uint8_t scc_read(uint32_t address);
void scc_write(uint32_t address, uint8_t value);

/* Debug: get and reset per-frame SCC read count */
uint32_t scc_get_read_count(void);
void scc_reset_read_count(void);

#ifdef __cplusplus
}
#endif
