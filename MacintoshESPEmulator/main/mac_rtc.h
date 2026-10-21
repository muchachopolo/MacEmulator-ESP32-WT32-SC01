#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Classic Mac RTC / PRAM chip (343-0042-B, Mac Plus/SE/Classic), serial on
 * VIA port B: PB0 = rtcData (bidirectional, DDRB bit 0), PB1 = rtcClk,
 * PB2 = /rtcEnb.  256 bytes of XPRAM.  Classic build only; the V8 build
 * keeps PRAM/clock in the Egret HLE.
 *
 * CLASSIC_RTC (default 1): emulate the chip.  0 = historical behaviour
 * (PB0 always reads 1, every PRAM byte $FF, clock frozen).
 */
#ifndef CLASSIC_RTC
#define CLASSIC_RTC 1
#endif

void macrtc_init(void);
/* Pin state after an ORB/DDRB write (VIA ORB and DDRB values). */
void macrtc_portb_changed(uint8_t orb, uint8_t ddrb);
/* Level the chip drives on PB0 (only meaningful while DDRB bit 0 = 0). */
int  macrtc_data_out(void);
/* Advance the seconds counter (one-second tick). */
void macrtc_one_second(void);
uint32_t macrtc_get_seconds(void);
const uint8_t *macrtc_get_pram(void);

#ifdef __cplusplus
}
#endif
