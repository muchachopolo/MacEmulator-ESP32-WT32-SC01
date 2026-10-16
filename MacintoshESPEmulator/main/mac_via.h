#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * VIA 6522 Emulation for Macintosh
 *
 * The Mac uses a single 6522 VIA for:
 *   - Overlay control (Port A bit 4) - maps ROM to address 0 on reset
 *   - VBlank interrupt (CA1)
 *   - One-second interrupt (CA2)
 *   - Keyboard clock and data (Port B bits 0-2)
 *   - Mouse button (not directly, but via SCC)
 *   - Sound volume (Port A bits 0-2)
 *   - Screen buffer select (Port A bit 6)
 *   - SCC wait/request (Port A bit 7)
 *
 * VIA registers are at 0xEFE1FE with 512-byte spacing:
 *   Register N is at 0xEFE1FE + N * 0x200
 */

/* VIA register indices */
#define VIA_REG_ORB     0   /* Output Register B / Input Register B */
#define VIA_REG_ORA     1   /* Output Register A (with handshake) */
#define VIA_REG_DDRB    2   /* Data Direction Register B */
#define VIA_REG_DDRA    3   /* Data Direction Register A */
#define VIA_REG_T1CL    4   /* Timer 1 Counter Low */
#define VIA_REG_T1CH    5   /* Timer 1 Counter High */
#define VIA_REG_T1LL    6   /* Timer 1 Latch Low */
#define VIA_REG_T1LH    7   /* Timer 1 Latch High */
#define VIA_REG_T2CL    8   /* Timer 2 Counter Low */
#define VIA_REG_T2CH    9   /* Timer 2 Counter High */
#define VIA_REG_SR      10  /* Shift Register */
#define VIA_REG_ACR     11  /* Auxiliary Control Register */
#define VIA_REG_PCR     12  /* Peripheral Control Register */
#define VIA_REG_IFR     13  /* Interrupt Flag Register */
#define VIA_REG_IER     14  /* Interrupt Enable Register */
#define VIA_REG_ORA_NH  15  /* Output Register A (no handshake) */

/* IFR/IER bit definitions */
#define VIA_IRQ_CA2     0x01    /* CA2 interrupt (one-second) */
#define VIA_IRQ_CA1     0x02    /* CA1 interrupt (VBlank) */
#define VIA_IRQ_SR      0x04    /* Shift register */
#define VIA_IRQ_CB2     0x08    /* CB2 interrupt */
#define VIA_IRQ_CB1     0x10    /* CB1 interrupt */
#define VIA_IRQ_T2      0x20    /* Timer 2 */
#define VIA_IRQ_T1      0x40    /* Timer 1 */
#define VIA_IRQ_ANY     0x80    /* Any interrupt (set if any enabled IRQ fires) */

void via_init(void);
void via_reset(void);

uint8_t via_read(uint32_t address);
void via_write(uint32_t address, uint8_t value);

/* Called every VBlank (~60Hz) to trigger CA1 interrupt */
void via_vblank(void);

/* Called every ~1 second to trigger CA2 interrupt (RTC one-second clock) */
void via_one_second(void);

/* Called periodically to advance timers. cycles = CPU cycles elapsed. */
void via_tick(int cycles);

/* Returns true if VIA has a pending interrupt */
bool via_irq_pending(void);

/* Get the overlay state (true = ROM mapped at address 0) */
bool via_get_overlay(void);

/* True once overlay has been OFF at least once (vectors copied to RAM) */
bool via_get_overlay_was_off(void);

/* Fast overlay access: avoids function call overhead in memory dispatch.
 * Updated in sync with via.overlay/via.overlay_was_off. */
extern bool via_overlay_fast;
extern bool via_overlay_was_off_fast;

/* Set mouse button state in VIA Port B (PB3, active low) */
void via_set_mouse_button(bool pressed);

/* Enable ADB shift register interrupt emulation (call after boot reaches INITs) */
void via_set_adb_active(bool active);

/* Debug getters for diagnostic logging */
uint8_t via_get_ifr(void);
uint8_t via_get_ier(void);
uint8_t via_get_acr(void);
uint16_t via_get_t1(void);
uint16_t via_get_t2(void);

/* Debug counter accessors */
uint32_t via_get_t2_fires(void);
uint32_t via_get_t2ch_writes(void);
uint32_t via_get_t2cl_reads(void);
uint16_t via_get_t2_peak_pending(void);
const uint32_t *via_get_read_counts(void);
const uint32_t *via_get_write_counts(void);
void via_reset_debug_counters(void);

#ifdef __cplusplus
}
#endif
