#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Macintosh Memory Map (24-bit address bus)
 *
 * With overlay = 0 (normal operation):
 *   0x000000 - 0x3FFFFF : RAM (4MB, mirrored if less installed)
 *   0x400000 - 0x4FFFFF : ROM (512KB, mirrored to fill 1MB)
 *   0x580000 - 0x5FFFFF : Phase-adjusted RAM (for testing, rarely used)
 *   0x600000 - 0x7FFFFF : Unused
 *   0x800000 - 0x9FFFFF : SCC read
 *   0xA00000 - 0xBFFFFF : SCC write
 *   0xC00000 - 0xDFFFFF : IWM/SWIM
 *   0xE00000 - 0xEFFFFF : VIA
 *   0xF00000 - 0xFFFFFF : Unused / Phase-adjusted
 *
 * With overlay = 1 (after reset):
 *   0x000000 - 0x0FFFFF : ROM (mapped here for reset vector access)
 *   0x100000 - 0x3FFFFF : ROM mirrors
 *   0x400000 - 0x4FFFFF : ROM (normal location)
 *   0x600000 - 0x6FFFFF : RAM (alternate access during overlay)
 *   Rest: same as above
 */

/* Initialize memory subsystem. rom_data/rom_size are the ROM image.
 * disk_data/disk_size are optional disk image. */
bool mac_mem_init(const uint8_t *rom_data, uint32_t rom_size,
                  uint8_t *disk_data, uint32_t disk_size);

/* Get direct pointer to Mac RAM (for video rendering) */
uint8_t *mac_mem_get_ram(void);

/* Get RAM size */
uint32_t mac_mem_get_ram_size(void);

/* Boot bypass: when true, the BEQ at $400CAC is replaced with NOP
 * so the ROM's boot loop proceeds to the _Read call. */
extern volatile bool mac_boot_read_bypass;

/* Sync internal SRAM shadow from PSRAM after bulk low-memory patches.
 * Call after installing exception vectors, trap tables, etc. */
void mac_mem_sync_fast_ram(void);

#ifdef __cplusplus
}
#endif
