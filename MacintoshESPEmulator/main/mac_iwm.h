#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * IWM/SWIM (Integrated Woz Machine) Emulation for Macintosh
 *
 * Handles floppy disk access. The Mac Classic uses the SWIM chip
 * which is backwards-compatible with IWM.
 *
 * IWM is at 0xC00000 - 0xDFFFFF
 * Registers are selected by address lines A0-A3 (active on odd bytes)
 *
 * For disk image support, we read from a .dsk file embedded in flash.
 */

/* Initialize IWM with an optional disk image */
void iwm_init(uint8_t *disk_data, uint32_t disk_size);

/* true: the image (read-only in flash) has a PSRAM copy-on-write overlay
 * (mac_disk_overlay.c, DISK_WRITABLE) and the volume mounts unlocked.
 * false: writes fail with wPrErr and the volume mounts locked. Set by
 * iwm_init(). */
extern bool iwm_disk_writable;

uint8_t iwm_read(uint32_t address);
void iwm_write(uint32_t address, uint8_t value);

/* Get direct pointer to disk image data and its size (for boot bypass) */
uint8_t *iwm_get_disk_data(void);
uint32_t iwm_get_disk_size(void);

/* Returns true if a disk is inserted */
bool iwm_disk_inserted(void);

/* Returns true if the SWIM is actively reading sector data */
bool iwm_is_reading(void);

/* Returns true if the SWIM is in ISM mode (vs IWM-compat mode).
 * Used by memory decode: when ISM is active, $EFxxxx reads with A12=1
 * go to SWIM instead of VIA (Mac Classic address sharing). */
bool iwm_is_ism_mode(void);

/* Insert the disk (deferred from init so ROM boots to '?' first) */
void iwm_insert_disk(void);

/* Sense line polling diagnostics */
int iwm_get_sense_poll_count(void);
int iwm_get_sense_poll_ism(void);

/* Advance IWM byte clock — call after each m68k_execute() with cycles consumed.
 * Accumulates cycles but does NOT fire RDDATA (only update_byte_clock does). */
void iwm_tick(int cycles);

/* Prepare byte clock for a new execution slice — call BEFORE m68k_execute().
 * Sets up cycle tracking so the first ISM access in the slice is counted. */
void iwm_start_slice(int cycles);

#ifdef __cplusplus
}
#endif
