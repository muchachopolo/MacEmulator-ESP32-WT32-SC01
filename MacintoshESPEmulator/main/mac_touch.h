#pragma once
/*
 * Touch panel -> trackpad gestures (shared by the Classic and V8 builds).
 *
 * Sampling is split from consumption:
 *   touch_sample(now_us)  reads the FT5x06 once and runs the gesture state
 *                         machine (swipe = move, tap = click, tap-then-drag =
 *                         click-and-drag). Timing uses now_us, the sample's
 *                         real timestamp.
 *   touch_consume(&rep)   called once per emulated frame by the emulation
 *                         loop; returns the movement since the last call and
 *                         the button state to present this frame.
 *
 * TOUCH_SAMPLER_TASK=1 (ESP32 default): touch_sampler_start() creates a small
 * FreeRTOS task on core 0 that calls touch_sample() at a steady ~60 Hz, so
 * sampling does not depend on emulation speed. The task is the only caller
 * of display_get_touch() (I2C). The shared state is guarded by a spinlock.
 * TOUCH_SAMPLER_TASK=0 (host default): touch_poll_inline() calls
 * touch_sample() from the emulation loop every TOUCH_POLL_US of wall clock.
 */
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TOUCH_SAMPLER_TASK
#if defined(HOST_BUILD) && HOST_BUILD
#define TOUCH_SAMPLER_TASK 0
#else
#define TOUCH_SAMPLER_TASK 1
#endif
#endif

/* 1 = LC II: a relative pointing device (touch_mouse_input) is fed to the emulated
 * ADB mouse as deltas, so the OS applies its own tracking curve (Mouse control
 * panel). 0 = its movement is added to the cursor position 1:1 like the touch
 * gestures. The touch panel always moves the cursor directly. */
#ifndef MAC_MOUSE_ADB
#define MAC_MOUSE_ADB 0
#endif
#if MAC_MOUSE_ADB && !MAC_ROM_LC_II
#undef MAC_MOUSE_ADB
#define MAC_MOUSE_ADB 0
#endif

#define TOUCH_POLL_US        16000   /* ~60 Hz sampling */
#define TAP_MOVE_THRESH      8       /* px: beyond this = drag */
#define TAP_TIME_US          300000  /* max tap duration */
#define TAP_SLOW_THRESH      3       /* px: drag threshold once past TAP_TIME_US */
#define TAP_DRAG_WINDOW_US   400000  /* second touch within this = tap-drag */
#define TOUCH_BTN_HOLD_FRAMES 2      /* min emulated frames per button state */

typedef struct {
    int  dx, dy;        /* screen-pixel movement to apply this frame */
    int  adb_dx, adb_dy;/* MAC_MOUSE_ADB: relative mouse counts for the ADB mouse */
    bool button;        /* button state to present this frame */
    bool btn_changed;   /* button differs from the previous frame */
    bool touching;      /* finger on panel (latest sample; for logging) */
    bool is_drag, tap_drag;
} touch_report_t;

/* Sample the panel once and update gesture state (one context only). */
void touch_sample(int64_t now_us);

/* Inline path (TOUCH_SAMPLER_TASK=0): rate-limited touch_sample() from the
 * emulation loop. No-op when the sampler task is enabled. */
void touch_poll_inline(int64_t now_us);

/* ESP32: start the sampler task (no-op when TOUCH_SAMPLER_TASK=0). */
void touch_sampler_start(void);

/* Emulation loop, once per frame. */
void touch_consume(touch_report_t *rep);

/* A relative pointing device besides the panel (Raspberry Pi port: USB
 * mouse): movement in screen pixels and the button state, merged into the
 * same report as touch gestures. Same context as touch_sample(). */
void touch_mouse_input(int dx, int dy, bool button);

#ifdef __cplusplus
}
#endif
