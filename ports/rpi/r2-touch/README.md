# R2 — touch test (Raspberry Pi 2 + AC-TFT35)

Bare-metal test of the display's XPT2046 resistive touch controller (SPI0 CE1,
PENIRQ = GPIO17, 1.95 MHz; it shares SPI0 with the ILI9486, which runs at
17.9 MHz). Phase R2 of [`spec/raspberry-pi-port.md`](../../../spec/raspberry-pi-port.md).
Drivers: `../common/ili9486.*`, `../common/xpt2046.*`.

Build and SD card: as in [R1](../r1-display/README.md) (`make` → `kernel7.img`).

## What you should see

1. **Calibration**: a white cross-hair in each corner in turn (top-left,
   top-right, bottom-left, bottom-right) with its number 1–4 in the middle. Press
   it firmly (a stylus or fingernail works best) for about half a second, then
   lift. Green = accepted, red = too short, press again.
2. **Result** for 6 s: five numbers — axis swap (0/1, yellow), then raw left,
   right, top and bottom (cyan).
3. **Drawing**: touch and drag to draw green lines. The top line shows raw X, Y
   and pressure while touched. The grey cross-hairs are where the targets were:
   touching them should hit their centres. The red box bottom-left clears the
   screen.
