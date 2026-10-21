#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Macintosh Classic Emulator
 *
 * Emulates a Macintosh Classic (68000 @ 7.83MHz):
 *   - 512x342 monochrome built-in display
 *   - Up to 4MB RAM (using ESP32 PSRAM)
 *   - 512KB ROM (Classic.ROM from flash)
 *   - VIA 6522 for keyboard, mouse, interrupts
 *   - IWM/SWIM for floppy disk
 *   - SCC for serial (stub)
 */

/* Mac Classic specifications */
#define MAC_CPU_HZ          7833600     /* 68000 clock: 7.8336 MHz */
#define MAC_SCREEN_WIDTH    512         /* pixels */
#define MAC_SCREEN_HEIGHT   342         /* pixels */
#define MAC_SCREEN_STRIDE   64          /* bytes per scanline (512/8) */
#define MAC_SCREEN_SIZE     (MAC_SCREEN_STRIDE * MAC_SCREEN_HEIGHT) /* 21888 bytes */

/* Classic native-LCD mode ("screen hack", Mini vMac precedent): ROM patches
 * shrink the Mac's VISIBLE screen to the LCD's 480x320 (CrsrPin, ScrnSize,
 * ScrnBitMap -> QuickDraw screenBits.bounds). The framebuffer keeps its
 * 512x342 / rowBytes 64 layout at ScrnBase; columns 480-511 and rows
 * 320-341 just become unused margin. The renderer then copies 1:1.
 * 0 = stock 512x342, scaled to the LCD. Ignored by the V8 (LC II) build,
 * which has its own native mode (mac_v8video.c). */
#ifndef CLASSIC_NATIVE_LCD
#define CLASSIC_NATIVE_LCD  1
#endif
#if CLASSIC_NATIVE_LCD && !MAC_ROM_LC_II
#define MAC_VIS_WIDTH       480
#define MAC_VIS_HEIGHT      320
#else
#define MAC_VIS_WIDTH       MAC_SCREEN_WIDTH
#define MAC_VIS_HEIGHT      MAC_SCREEN_HEIGHT
#endif

/* RAM/ROM sizes */
#ifndef MAC_RAM_SIZE        /* overridable for host experiments */
#define MAC_RAM_SIZE        (2 * 1024 * 1024)   /* 2MB RAM (power-of-2 for correct ROM sizing) */
#endif
#define MAC_ROM_SIZE        (512 * 1024)         /* 512KB ROM */

/* Timing */
#define MAC_VBLANK_HZ       60          /* VBlank rate ~60.15 Hz */
#define MAC_CYCLES_PER_VBLANK (MAC_CPU_HZ / MAC_VBLANK_HZ) /* ~130,560 */

/* Screen buffer is at the top of RAM */
#define MAC_SCREEN_BASE     (MAC_RAM_SIZE - 0x5900)  /* 2MB: 0x1FA700 */
#define MAC_SOUND_BASE      (MAC_RAM_SIZE - 0x0300)  /* 2MB: 0x1FFD00 */

/* Initialize the emulator (loads ROM, allocates RAM) */
bool mac_emu_init(void);

/* Run the emulator main loop (call from a dedicated task) */
void mac_emu_run(void *param);

/* Get pointer to the Mac screen framebuffer in RAM */
const uint8_t *mac_emu_get_screen(void);

/* Update mouse position (called from touch handler) */
void mac_emu_set_mouse(int x, int y, bool button);

/* Musashi callback declarations (must be extern "C") */
int mac_int_ack_handler(int level);
void mac_reset_handler(void);

#ifdef __cplusplus
}
#endif
