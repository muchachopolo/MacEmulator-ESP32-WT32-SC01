#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Egret/Cuda HLE (V8 family; Color Classic = Cuda). The MCU talks to the
 * CPU over VIA1: PB3 = TREQ (MCU -> CPU, active low), PB4 = BYTEACK and
 * PB5 = TIP (CPU -> MCU; active high on Egret, active low on Cuda).
 * Bytes move through the VIA shift register (ACR bit 4: 1 = CPU shifts out).
 */

/* 1 = Color Classic (Cuda), 0 = LC / LC II (Egret); set by the build */
#ifndef MAC_V8_CUDA
#define MAC_V8_CUDA 0
#endif

void egret_init(void);

/* VIA port B output register or direction changed */
void egret_portb_changed(uint8_t orb, uint8_t ddrb);

/* Advance pending Egret actions by CPU cycles */
void egret_tick(int cycles);

/* Called once per emulated second (RTC) */
void egret_one_second(void);

/* ADB mouse (address 3, handler 1): add a relative movement (Mac pixels,
 * +x right, +y down) and set the button state. Deltas accumulate and are
 * sent in autopoll packets of at most +-63 per axis; button transitions
 * are queued so a short click is never lost. */
void egret_adb_mouse(int dx, int dy, bool button);

/* True while a queued button transition has not yet been packed into an
 * autopoll packet. */
bool egret_adb_mouse_btn_pending(void);

#ifdef __cplusplus
}
#endif
