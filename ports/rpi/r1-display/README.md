# R1 — display bring-up test (Raspberry Pi 2 + AC-TFT35)

Bare-metal test of the 3.5" "RPi LCD (A)"-type display (ILI9486 behind 16-bit
shift registers, SPI0 CE0, DC = GPIO24, RESET = GPIO25). Phase R1 of
[`spec/raspberry-pi-port.md`](../../../spec/raspberry-pi-port.md).

## Build

```bash
# once: Arm GNU toolchain (arm-none-eabi) and Circle, configured for the Pi 2
git clone https://github.com/rsta2/circle ~/tools/circle
printf 'RASPPI = 2\nPREFIX = /path/to/arm-none-eabi-\n' > ~/tools/circle/Config.mk
(cd ~/tools/circle && ./makeall --nosample)

make            # → kernel7.img (driver: ../common/ili9486.*)   (CIRCLEHOME defaults to ~/tools/circle)
```

## SD card (FAT)

Copy `bootcode.bin`, `start.elf`, `fixup.dat` (Raspberry Pi firmware, e.g. from
github.com/raspberrypi/firmware `boot/`), `kernel7.img` and a `config.txt`:

```
arm_64bit=0
kernel=kernel7.img
```

## What you should see

1. For each pixel clock (17.9, 20.8, 25.0, 31.2, 41.7, 62.5 MHz; commands always
   at 15.6 MHz): fast red/blue flashing (timing run), then a 4-second **test
   pattern** with the clock printed in the grey band: colour bars
   white-yellow-cyan-green-magenta-red-blue-black, a smooth colour ramp (R, G, B,
   grey) above the grey band, a 1-px white border, corner squares **red top-left,
   green top-right, blue bottom-left, white bottom-right**.
2. A **summary**: one row per clock — clock (yellow), bar ∝ frame rate, fps (white).
3. Then the pattern repeats at 17.9 MHz.

## Result (2026-10-21, AC-TFT35-V2.1)

| SPI clock | Result | Full frame |
|---|---|---|
| 15.6 MHz | clean | 6.3 fps |
| **17.9 MHz** | **clean — the limit** | **7.2 fps** |
| 20.8 MHz | pixels dropped (ramp breaks into shifted segments); with commands at the same clock, windows also land in the wrong place | 8.4 fps |
| 25 – 62.5 MHz | worse: missing fills, noise, colour errors | 10 – 11.7 fps (the 11.7 cap is Circle's polled SPI) |

A plain fill hides a dropped pixel; only the ramp shows it. The 74HC4094/4040
shift-register chain is the bottleneck, so the driver uses 17.9 MHz
(`CILI9486::MaxClock`) for everything.

Logs also go to HDMI and the serial port (GPIO14/15, 115200 baud) if connected.
