/*
 * Touch panel -> trackpad gestures. See mac_touch.h for the design.
 *
 * Gestures (unchanged from the old in-loop code, except where noted):
 *  - swipe (> TAP_MOVE_THRESH px from touch-down): moves the cursor, no button.
 *    Once the threshold is crossed, the movement since touch-down is applied
 *    too (it used to be discarded, an 8 px dead zone on every swipe).
 *    After TAP_TIME_US the touch can no longer be a tap, so a slow nudge of
 *    TAP_SLOW_THRESH px is enough.
 *  - tap (< TAP_TIME_US, never crossed the threshold): click (down + up).
 *  - tap-then-drag: touch again within TAP_DRAG_WINDOW_US of a tap: button
 *    held for the whole touch. (The old code's pending tap release could drop
 *    the button one poll into the drag; button transitions are now queued, so
 *    the tap's up is ordered before the drag's down.)
 *
 * Button transitions go through a small queue; each queued transition carries
 * the movement that preceded it, so the consumer replays them in order and a
 * click lands where the finger was when it happened. The consumer presents
 * each button state for at least TOUCH_BTN_HOLD_FRAMES emulated frames, so a
 * tap that starts and ends between two frames is still seen by the Mac.
 */
#include "mac_touch.h"
#include "display.h"
#include <string.h>
#include <esp_log.h>
#include <esp_timer.h>

#if TOUCH_SAMPLER_TASK
#if defined(HOST_BUILD) && HOST_BUILD
#error "TOUCH_SAMPLER_TASK needs FreeRTOS; the host harness uses the inline path"
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
static portMUX_TYPE touch_mux = portMUX_INITIALIZER_UNLOCKED;
#define T_LOCK()   taskENTER_CRITICAL(&touch_mux)
#define T_UNLOCK() taskEXIT_CRITICAL(&touch_mux)
#else
#define T_LOCK()   do { } while (0)
#define T_UNLOCK() do { } while (0)
#endif

static const char *TAG = "touch";

/* ---- shared between sampler and consumer (under T_LOCK) ---- */
#define TQ_LEN 16
static struct {
    int acc_dx, acc_dy;             /* movement after the last queued transition */
    struct { int dx, dy; bool btn; } q[TQ_LEN];
    int qh, qn;
    bool touching, is_drag, tap_drag;
} sh;

/* ---- sampler-only gesture state ---- */
static struct {
    bool was_touching;
    int last_tx, last_ty, start_tx, start_ty;
    int64_t touch_start_us;
    int64_t last_tap_us;            /* time of last tap release, 0 = none */
    bool is_drag, tap_drag;
    bool btn;                       /* last requested button state */
} g;

/* ---- consumer-only state ---- */
static struct {
    bool btn;
    int hold;
} c;

/* Caller holds T_LOCK. */
static void push_btn(bool b)
{
    if (b == g.btn) return;
    g.btn = b;
    if (sh.qn >= TQ_LEN) return;    /* never in practice: consumer runs every frame */
    int i = (sh.qh + sh.qn) % TQ_LEN;
    sh.q[i].dx = sh.acc_dx;
    sh.q[i].dy = sh.acc_dy;
    sh.q[i].btn = b;
    sh.acc_dx = sh.acc_dy = 0;
    sh.qn++;
}

void touch_sample(int64_t now_us)
{
    int tx = 0, ty = 0;
    bool touch = display_get_touch(&tx, &ty);   /* I2C: outside the lock */

    T_LOCK();
    if (touch) {
        if (!g.was_touching) {
            /* finger down */
            g.touch_start_us = now_us;
            g.start_tx = tx;
            g.start_ty = ty;
            g.is_drag = false;
            if (g.last_tap_us > 0 && now_us - g.last_tap_us < TAP_DRAG_WINDOW_US) {
                g.tap_drag = true;          /* click-and-drag */
                push_btn(true);
            } else {
                g.tap_drag = false;
            }
        } else {
            /* Panel -> screen: axes swapped, horizontal inverted. */
            int dx, dy;
            if (!g.is_drag) {
                int tdx = -(ty - g.start_ty);
                int tdy = tx - g.start_tx;
                int d2 = tdx * tdx + tdy * tdy;
                /* Past the tap time a touch can no longer be a tap, so a
                 * small, slow nudge moves the cursor too (the 3 px floor
                 * rejects panel jitter during a long press). */
                if (d2 > TAP_MOVE_THRESH * TAP_MOVE_THRESH ||
                    (now_us - g.touch_start_us >= TAP_TIME_US &&
                     d2 >= TAP_SLOW_THRESH * TAP_SLOW_THRESH)) {
                    g.is_drag = true;
                    dx = tdx;               /* everything since touch-down */
                    dy = tdy;
                } else {
                    dx = dy = 0;
                }
            } else {
                dx = -(ty - g.last_ty);
                dy = tx - g.last_tx;
            }
            sh.acc_dx += dx;
            sh.acc_dy += dy;
        }
        g.was_touching = true;
        g.last_tx = tx;
        g.last_ty = ty;
    } else if (g.was_touching) {
        /* finger lifted */
        if (!g.is_drag && now_us - g.touch_start_us < TAP_TIME_US) {
            push_btn(true);                 /* tap: click (no-op if tap-drag) */
            push_btn(false);
            g.last_tap_us = now_us;
        } else {
            push_btn(false);                /* drag / long press ended */
            g.tap_drag = false;
            g.last_tap_us = 0;
        }
        g.was_touching = false;
    }
    sh.touching = touch;
    sh.is_drag = g.is_drag;
    sh.tap_drag = g.tap_drag;
    T_UNLOCK();
}

void touch_mouse_input(int dx, int dy, bool button)
{
    T_LOCK();
    sh.acc_dx += dx;
    sh.acc_dy += dy;
    push_btn(button);
    T_UNLOCK();
}

void touch_consume(touch_report_t *rep)
{
    memset(rep, 0, sizeof(*rep));
    T_LOCK();
    if (c.hold > 0) c.hold--;
    if (sh.qn > 0) {
        if (c.hold == 0) {
            /* next transition, with the movement that preceded it */
            rep->dx = sh.q[sh.qh].dx;
            rep->dy = sh.q[sh.qh].dy;
            c.btn = sh.q[sh.qh].btn;
            sh.qh = (sh.qh + 1) % TQ_LEN;
            sh.qn--;
            rep->btn_changed = true;
            c.hold = TOUCH_BTN_HOLD_FRAMES;
        }
        /* else: hold the current state; later movement waits its turn */
    } else {
        rep->dx = sh.acc_dx;
        rep->dy = sh.acc_dy;
        sh.acc_dx = sh.acc_dy = 0;
    }
    rep->touching = sh.touching;
    rep->is_drag = sh.is_drag;
    rep->tap_drag = sh.tap_drag;
    T_UNLOCK();
    rep->button = c.btn;
}

void touch_poll_inline(int64_t now_us)
{
#if !TOUCH_SAMPLER_TASK
    static int64_t last_poll_us;
    if (now_us - last_poll_us > TOUCH_POLL_US) {
        last_poll_us = now_us;
        touch_sample(now_us);
    }
#else
    (void)now_us;
#endif
}

#if TOUCH_SAMPLER_TASK
#define TOUCH_TASK_CORE     0   /* with mac_video; core 1 is the emulator */
#define TOUCH_TASK_PRIORITY (configMAX_PRIORITIES - 2)  /* above mac_video */
#define TOUCH_TASK_STACK    3072

static void touch_task(void *param)
{
    (void)param;
    TickType_t period = pdMS_TO_TICKS(TOUCH_POLL_US / 1000);
    if (period == 0) period = 1;
    TickType_t last = xTaskGetTickCount();
    ESP_LOGI(TAG, "touch sampler on core %d, every %u ticks",
             xPortGetCoreID(), (unsigned)period);
    for (;;) {
        touch_sample(esp_timer_get_time());
        xTaskDelayUntil(&last, period);
    }
}
#endif

void touch_sampler_start(void)
{
#if TOUCH_SAMPLER_TASK
    xTaskCreatePinnedToCore(touch_task, "touch", TOUCH_TASK_STACK, NULL,
                            TOUCH_TASK_PRIORITY, NULL, TOUCH_TASK_CORE);
#else
    ESP_LOGI(TAG, "touch sampled inline from the emulation loop");
#endif
}
