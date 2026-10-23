/*
 * Apple Sound Chip (ASC) of the LC II, $50F14000-$50F15FFF (LC II only).
 * FIFO A $000-$3FF, FIFO B $400-$7FF, registers $800+. Only FIFO (sampled)
 * mode is emulated: the Sound Manager and the start-up chime use it. The
 * 68k side runs in device time (asc_tick); the platform pulls 44.1 kHz
 * stereo samples with asc_pull() from whatever audio output it has.
 */
#ifndef MAC_ASC_H
#define MAC_ASC_H

#include <stdint.h>
#include <stdbool.h>

#ifndef MAC_SOUND
#define MAC_SOUND 0     /* 1 = ASC emulation + audio samples (LC II only) */
#endif
#if MAC_SOUND && !MAC_ROM_LC_II
#undef MAC_SOUND
#define MAC_SOUND 0
#endif

#define ASC_OUT_RATE 44100

#ifdef __cplusplus
extern "C" {
#endif

void asc_init(void);
void asc_set_enabled(bool on);      /* false: the chip reads as absent ($FF), no samples */
uint8_t asc_read(uint32_t address);
void asc_write(uint32_t address, uint8_t value);

/* Advance the chip by device cycles (MAC_CPU_HZ clock, not CPU cycles). */
void asc_tick(uint32_t device_cycles);

/* Consumer side: copy up to `frames` interleaved L/R int16 frames, returns
 * how many were available (the caller pads with silence). */
uint32_t asc_pull(int16_t *stereo, uint32_t frames);

/* true while more than ~46 ms of audio is queued for the consumer: the
 * emulator must not run ahead of real time then (boot turbo). */
bool asc_backlog(void);

#ifdef __cplusplus
}
#endif

#if !MAC_SOUND
#define asc_backlog() false
#define asc_set_enabled(on) ((void)(on))
#endif

#endif
