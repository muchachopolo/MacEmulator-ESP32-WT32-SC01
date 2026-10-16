#pragma once

#include <stdint.h>
#include <stdbool.h>

#define DISPLAY_WIDTH   480
#define DISPLAY_HEIGHT  320

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the display panel, touch controller, and backlight */
bool display_init(void);

/* Draw a rectangular region of RGB565 pixels to the display.
 * Data must be in big-endian RGB565 format (as expected by ST7796 over SPI).
 * This call is blocking until DMA transfer is complete. */
void display_draw(int x, int y, int w, int h, const uint16_t *data);

/* Read touch input. Returns true if touch is active.
 * x, y are in display coordinates (0..479, 0..319). */
bool display_get_touch(int *x, int *y);

/* Set backlight brightness (0-100) */
void display_set_brightness(int percent);

#ifdef __cplusplus
}
#endif
