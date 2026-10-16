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

/* RAM/ROM sizes */
#define MAC_RAM_SIZE        (2 * 1024 * 1024)   /* 2MB RAM (power-of-2 for correct ROM sizing) */
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
