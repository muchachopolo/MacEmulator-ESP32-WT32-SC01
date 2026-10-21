#pragma once

/*
 * Writable boot disk, "level 1": copy-on-write overlay in PSRAM.
 *
 * The disk image stays read-only in flash.  Each 512-byte block that the Mac
 * writes is copied into a slot of a PSRAM pool; reads take the slot when the
 * block has one, otherwise the flash image.  Everything written is lost at
 * power-off / restart (the next boot sees the pristine image again).
 *
 *   DISK_WRITABLE    1 (default): volume mounts unlocked, writes go to the
 *                    overlay.  0: the old read-only behaviour exactly
 *                    (writeProt set, writes fail / never reach the image,
 *                    no PSRAM allocated).
 *   DISK_OVERLAY_KB  pool size (default 384 Classic, 256 LC II, whose VRAM
 *                    takes 512 KB of PSRAM).  Cost = pool + 2 bytes/block
 *                    index (11.25 KB for a 2880K image), both in PSRAM.
 *
 * A write that needs more new slots than are free fails as a whole with
 * dskFulErr (-34): nothing is written, so the image never holds half a
 * request.  The File Manager passes the error up ("disk is full").
 * Writes of data identical to the flash image do not take a slot.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DISK_WRITABLE
#define DISK_WRITABLE 1
#endif

#ifndef DISK_OVERLAY_KB
#if MAC_ROM_LC_II
#define DISK_OVERLAY_KB 256
#else
#define DISK_OVERLAY_KB 384
#endif
#endif

#define DISK_OV_BLOCK   512
#define DISK_OV_DSKFUL  (-34)     /* dskFulErr */

/* Pool size used by disk_ov_init(); the host harness raises it for
 * --disk-rw (whole image) before mac_emu_init(). */
extern uint32_t disk_ov_pool_kb;

/* Allocate the overlay for an image.  Returns true when writes are
 * possible (DISK_WRITABLE and the allocation succeeded). */
bool disk_ov_init(const uint8_t *img, uint32_t size);

/* true after a successful disk_ov_init() */
bool disk_ov_active(void);

/* Copy cnt bytes at image offset off into dst (overlay-aware).
 * The caller checks the range. */
void disk_ov_read(uint8_t *dst, uint32_t off, uint32_t cnt);

/* One byte of the (overlaid) image */
uint8_t disk_ov_byte(uint32_t off);

/* Write cnt bytes at image offset off.  Returns 0 or DISK_OV_DSKFUL
 * (pool full: nothing written).  Requires disk_ov_active(). */
int disk_ov_write(const uint8_t *src, uint32_t off, uint32_t cnt);

/* Transfer for a .Sony request found in RAM: write when `write` and the
 * overlay is active, else read (the old read-only paths copied the image
 * into the buffer for writes too; DISK_WRITABLE=0 keeps that). */
int disk_ov_xfer(bool write, uint8_t *rambuf, uint32_t off, uint32_t cnt);

/* Log usage (blocks used / pool, writes, full errors) */
void disk_ov_log_stats(const char *why);

#ifdef __cplusplus
}
#endif
