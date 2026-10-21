/* Host stand-in for components/display: captures display_draw() strips
 * into a 480x320 RGB565 framebuffer; touch comes from --tap / --swipe scripts. */
#include "display.h"
#include <string.h>

uint16_t host_lcd[DISPLAY_WIDTH * DISPLAY_HEIGHT];

bool display_init(void) { return true; }
void display_set_brightness(int percent) { (void)percent; }
/* Scripted taps (--tap F): touch is held from frame F for HOST_TAP_FRAMES
 * frames. The emulator polls touch by wall clock (16 ms) and classifies taps
 * by duration (<300 ms), so the hold spans a few polls at host speed. */
#define HOST_TAP_FRAMES 300
long host_taps[16];
int  host_ntaps;
extern long host_frame_count(void);

/* Scripted swipes (--swipe F:dx:dy[:N]): touch held from frame F for N
 * frames (default 2000), the point moving linearly so the Mac cursor moves
 * by (dx, dy) screen pixels before acceleration. Polls happen every 16 ms of
 * wall clock (~100+ frames on the host), so each poll sees a partial step.
 * The emulator maps touch -> screen as dx = -(ty delta), dy = +(tx delta). */
#define HOST_SWIPE_REST 400
struct host_swipe { long f, n; int dx, dy; };
struct host_swipe host_swipes[16];
int host_nswipes;

bool display_get_touch(int *x, int *y)
{
    long f = host_frame_count();
    for (int i = 0; i < host_ntaps; i++) {
        if (f >= host_taps[i] && f < host_taps[i] + HOST_TAP_FRAMES) {
            *x = 160; *y = 240;     /* fixed point; a tap does not move */
            return true;
        }
    }
    for (int i = 0; i < host_nswipes; i++) {
        const struct host_swipe *s = &host_swipes[i];
        if (f >= s->f && f < s->f + s->n) {
            /* Rest HOST_SWIPE_REST frames at each end (> one 16 ms poll),
             * so the first and last polls see the exact start and end
             * points and the cursor travels exactly (dx, dy). */
            long rest = s->n > 3 * HOST_SWIPE_REST ? HOST_SWIPE_REST : 0;
            long k = f - s->f - rest, d = s->n - 2 * rest - 1;
            if (d < 1) d = 1;
            if (k < 0) k = 0; else if (k > d) k = d;
            *x = 160 + (int)(s->dy * k / d);
            *y = 240 - (int)(s->dx * k / d);
            return true;
        }
    }
    return false;
}

void display_draw(int x, int y, int w, int h, const uint16_t *data)
{
    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= DISPLAY_HEIGHT) continue;
        for (int col = 0; col < w; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= DISPLAY_WIDTH) continue;
            host_lcd[dy * DISPLAY_WIDTH + dx] = data[row * w + col];
        }
    }
}
