#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * V8 (Color Classic / LC II) built-in video hardware, MAC_ROM_LC_II only.
 *
 *   $F24000  VDAC (CLUT): +0 address, +1 data (R,G,B triplets), +2 control
 *   $F26000  pseudo-VIA2 / RBV-style registers:
 *              $00 DataB  $01 Exp  $02 SlotIFR (active low, bit 6 = video VBL)
 *              $03 IFR    $10 MonP (bits 0-2 depth, 3-5 monitor sense (RO),
 *                                   bit 6 video off)
 *              $11 ChpT   $12 SlotIER  $13 IER
 *   $F40000  VRAM, 512KB ($F40000-$FBFFFF)
 *
 * Built-in display on the Color Classic: 512x384, Apple sense code 2
 * ("12-inch RGB").
 */

/* 1 = patch the 512x384 mode to the LCD's native 480x320 (1:1 render) */
#ifndef V8_NATIVE_LCD
#define V8_NATIVE_LCD  1
#endif

#define V8_VRAM_BASE   0xF40000
#define V8_VRAM_SIZE   0x80000

bool     v8video_init(void);
uint8_t  v8video_read(uint32_t address);
void     v8video_write(uint32_t address, uint8_t value);
void     v8video_vbl(bool active);
/* Pseudo-VIA2 IFR & IER != 0 (CPU IRQ level 2) */
bool     v8video_irq_pending(void);
/* Build the native-LCD declaration-ROM copy. Returns the ROM file offset
 * where *copy starts (byte reads at >= that offset use the copy), or
 * 0xFFFFFFFF when nothing is patched. */
uint32_t v8video_patch_rom(const uint8_t *rom, uint32_t size, const uint8_t **copy);

/* For the renderer / diagnostics */
uint8_t *v8video_get_vram(void);
uint8_t  v8video_get_monp(void);               /* RBV $10: depth in bits 0-2 */
const uint8_t *v8video_get_clut(void);         /* 256 x {R,G,B} */
/* CLUT as RGB565 in LCD wire order; generation bumps on every CLUT write */
const uint16_t *v8video_get_pal565(void);
uint32_t v8video_get_pal_gen(void);
/* Active built-in display mode in pixels (512x384, or 480x320 when the
 * native-LCD ROM patch is applied) */
int v8video_screen_width(void);
int v8video_screen_height(void);
/* Current depth: log2(bits per pixel), 0..3 (4 = 16bpp) from MonP bits 0-2 */
static inline int v8video_depth_log2(void) { return v8video_get_monp() & 7; }

#ifdef __cplusplus
}
#endif
