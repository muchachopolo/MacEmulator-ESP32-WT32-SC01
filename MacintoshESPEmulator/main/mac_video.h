#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Mac Video - Converts 512x342 1-bit framebuffer to 480x320 RGB565
 *
 * The Mac Classic has a built-in monochrome display:
 *   - 512 x 342 pixels, 1 bit per pixel
 *   - Framebuffer is at top of RAM: RAM_SIZE - 0x5900
 *   - Each scanline is 64 bytes (512 / 8)
 *   - Bit 7 = leftmost pixel, bit 0 = rightmost
 *   - 1 = black, 0 = white
 *
 * We scale this to the WT32-SC01 display (480x320) using
 * precomputed nearest-neighbor lookup tables.
 */

/* Classic GAME_FULL_VIEW: set by the emulation loop (core 1) while a
 * full-screen game draws outside 480x320; the renderer (core 0) then shows
 * the whole 512x342 buffer scaled to 480x320 instead of the 1:1 480x320. */
extern volatile bool mac_full_view;

/* Initialize video subsystem (precompute scale tables, allocate line buffer) */
bool mac_video_init(void);

/* Render the Mac framebuffer to the display.
 * mac_ram = pointer to Mac RAM base
 * ram_size = total RAM size (needed to find screen base)
 * Sends the converted image to the display via SPI. */
void mac_video_render(const uint8_t *mac_ram, uint32_t ram_size);

/* FreeRTOS task that renders video continuously on a separate core.
 * Decouples video from CPU emulation for maximum throughput. */
void mac_video_task(void *param);

#ifdef __cplusplus
}
#endif
