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

make            # → kernel7.img   (CIRCLEHOME defaults to ~/tools/circle)
```

## SD card (FAT)

Copy `bootcode.bin`, `start.elf`, `fixup.dat` (Raspberry Pi firmware, e.g. from
github.com/raspberrypi/firmware `boot/`), `kernel7.img` and a `config.txt`:

```
arm_64bit=0
kernel=kernel7.img
```

## What you should see

1. For each SPI clock (15.6, 31.2, 41.7, 62.5 MHz): fast red/blue flashing (timing
   run), then a 4-second **test pattern** with the clock printed in the grey band:
   colour bars white-yellow-cyan-green-magenta-red-blue-black, a 1-px white border,
   corner squares **red top-left, green top-right, blue bottom-left, white
   bottom-right**. The highest clock whose pattern still looks clean is usable.
2. A **summary**: one row per clock — clock (yellow), bar ∝ frame rate, fps (white).
3. Then the pattern repeats (drawn at the safe 15.6 MHz).

Logs also go to HDMI and the serial port (GPIO14/15, 115200 baud) if connected.
