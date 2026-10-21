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

/* Classic build: drive the CPU's level-1 IRQ line from every VIA IFR/IER
 * change (as the V8 build does), not only at the end of each execution
 * slice. With the line only lowered at slice end, the CPU re-took the
 * already-cleared VIA interrupt each time a handler lowered the SR mask
 * (~14 level-1 interrupts per frame instead of ~1-2). 0 = old behaviour,
 * for board A/B. */
#ifndef CLASSIC_VIA_IRQ_NOW
#define CLASSIC_VIA_IRQ_NOW 1
#endif
/* Classic full view for full-screen games (GAME_FULL_VIEW, default 1; only
 * with CLASSIC_NATIVE_LCD on the Classic build, needs TOUCH_GAME_MODE).
 * While touch game mode is active (cursor hidden or a windowless app in
 * front, with hysteresis) AND the game has drawn into the 512x342 margin
 * (columns 480-511 / rows 320-341), the renderer shows the whole 512x342
 * buffer scaled to 480x320, CrsrPin ($0834) is widened to 0,0,342,512 and
 * touch movement is scaled by 512/480 x 342/320. Leaving game mode returns
 * to the crisp 1:1 480x320 view (mac_emulator.c full_view_track()).
 * GAME_FULL_SCREENBITS (default = GAME_FULL_VIEW): apps whose name starts
 * with an entry of the list in mac_memory.c (a full-screen game, Prince of
 * Persia ...) get the stock 512x342 from _ScrnBitMap / ScrnSize, so a game
 * that centres its fixed 512x342 field on screenBits (a full-screen game) is not
 * cut off at all four edges. */
#ifndef GAME_FULL_VIEW
#define GAME_FULL_VIEW 1
#endif
#if !CLASSIC_NATIVE_LCD || MAC_ROM_LC_II
#undef  GAME_FULL_VIEW
#define GAME_FULL_VIEW 0
#endif
#ifndef GAME_FULL_SCREENBITS
#define GAME_FULL_SCREENBITS GAME_FULL_VIEW
#endif
#if !CLASSIC_NATIVE_LCD || MAC_ROM_LC_II
#undef  GAME_FULL_SCREENBITS
#define GAME_FULL_SCREENBITS 0
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

/* Device clock vs CPU clock (V8 / LC II build).
 * MAC_CYCLES_PER_VBLANK is the DEVICE time of one frame: VIA E clock
 * (cycles/10), Egret, VBL.  The CPU gets MAC_CPU_CYCLES_PER_FRAME =
 * MAC_CYCLES_PER_VBLANK / MAC_CYCLE_SCALE Musashi cycles per frame, and
 * each executed CPU cycle advances device time by MAC_CYCLE_SCALE.
 * Why: Musashi's 68030 timings average ~6.2 cycles/instruction vs ~12.8 for
 * the 68000, so at the same 7.8336 MHz budget the LC II ran ~20,800
 * instructions per tick vs ~10,200 on the Classic.  The UI is tick-paced
 * (menu blink, drag, Delay, WaitNextEvent sleep), so that doubled the host
 * work per tick.  Scale 2 = a 68030 at ~3.9 MHz: about the Classic's
 * instruction throughput per tick, while timers, Ticks and the ROM's
 * VIA-based calibrations (TimeDBRA etc.) stay on the real time scale.
 * SWIM byte timing (mac_iwm.c) stays in CPU cycles on purpose (the CPU's
 * view of byte arrival is unchanged).  1 = previous behaviour.
 * The Classic build always uses 1. */
#ifndef MAC_V8_CYCLE_SCALE
#define MAC_V8_CYCLE_SCALE  2
#endif
#if MAC_ROM_LC_II
#define MAC_CYCLE_SCALE     MAC_V8_CYCLE_SCALE
#else
#define MAC_CYCLE_SCALE     1
#endif
#define MAC_CPU_CYCLES_PER_FRAME (MAC_CYCLES_PER_VBLANK / MAC_CYCLE_SCALE)

/* Idle fast-forward (mac_emulator.c): when the Mac is provably only waiting
 * for an interrupt (Process Manager idle loop with no interrupt since the
 * previous pass, or a "CMP.L Ticks,Dn / Bcc.S *-4" tick-wait loop), stop
 * emulating the spin and advance device time (VIA/Egret/VBL) until an
 * interrupt the CPU will take is pending, or the frame ends.
 * 0 = off (A/B). */
#ifndef MAC_IDLE_FF
#define MAC_IDLE_FF         1
#endif

/* Real-time throttle: never run more than one frame per MAC_FRAME_US of
 * wall clock (frames become cheap with MAC_IDLE_FF).  Off on the host
 * harness so host runs stay fast. */
#ifndef MAC_FRAME_THROTTLE
#ifdef HOST_BUILD
#define MAC_FRAME_THROTTLE  0
#else
#define MAC_FRAME_THROTTLE  1
#endif
#endif
#define MAC_FRAME_US        16626       /* 60.15 Hz */

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

/* Idle fast-forward hooks (mac_emulator.c) */
void mac_aline_hook(unsigned int trap_word, unsigned int trap_pc);
void mac_idle_note_irq(void);

#ifdef __cplusplus
}
#endif
