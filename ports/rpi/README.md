# Raspberry Pi 2 port (bare metal)

The emulator core of this repository (`MacintoshESPEmulator/main` + Musashi,
unmodified) running without an operating system on a **Raspberry Pi 2 Model B**
with a **3.5" 480×320 SPI touch display** ("3.5inch RPi LCD (A)" design, e.g.
AC-TFT35-V2.1). The Pi boots straight into the Mac from the SD card.

How it works and what was learned: [`docs/learn/09-raspberry-pi-port.md`](../../docs/learn/09-raspberry-pi-port.md).
Plan and status: [`spec/raspberry-pi-port.md`](../../spec/raspberry-pi-port.md).

| Folder | Contents |
|---|---|
| [`macemu/`](macemu/README.md) | The emulator kernel (`kernel7.img` Classic, `kernel7-lc2.img` LC II) |
| [`common/`](common) | ILI9486 display and XPT2046 touch drivers |
| [`r1-display/`](r1-display/README.md) | Display bring-up test (SPI clock measurement) |
| [`r2-touch/`](r2-touch/README.md) | Touch calibration test |

## What you get

- **Mac Classic** (68000, black and white) or **LC II** (68030, colour, 8 MB RAM,
  full CPU speed), chosen with one line on the SD card.
- **Disks on the SD card**, loaded into RAM on demand and in the background;
  changes are **saved back** to the image files (they survive power-off).
- **Two drives** on the LC II (`disk=` boot disk, `disk2=` second disk).
- **USB keyboard and mouse** (Alt = Option, Windows key = Command), plus the
  touch screen as a trackpad (swipe, tap, tap-then-drag).
- **Turbo boot**: no real-time cap until the Finder is up.
- Four CPU cores used: core 0 runs the 68k, core 1 drives the LCD and the touch
  panel, core 2 does all SD card access, core 3 is free.

## Hardware

| Part | Notes |
|---|---|
| Raspberry Pi 2 Model B (v1.1) | BCM2836, 4× Cortex-A7 900 MHz, 1 GB |
| 3.5" SPI LCD, ILI9486 behind 74HC4094 shift registers | DC = GPIO24, RESET = GPIO25, SPI0 CE0; **17.9 MHz maximum** on the tested unit |
| XPT2046 resistive touch | SPI0 CE1, PENIRQ = GPIO17; calibration in `common/xpt2046.h` |
| SD card | FAT32. **Use a genuine card**: a fake "2 GB" card that really stored ~128 MB corrupted large files |
| Optional | USB keyboard, USB mouse |

## SD card

```
/bootcode.bin /start.elf /fixup.dat   Raspberry Pi firmware (github.com/raspberrypi/firmware, boot/)
/config.txt                           arm_64bit=0
                                      kernel=kernel7.img      (Mac Classic)
                                   or kernel=kernel7-lc2.img  (LC II)
/kernel7.img /kernel7-lc2.img         built from macemu/
/mac/Classic.ROM /mac/LC-II.ROM       your ROMs
/mac/<disk images>                    bare HFS images (.dsk), any size
/mac/config.txt                       settings, see below
```

`/mac/config.txt` (one `key=value` per line, `#` comments):

| Setting | Meaning |
|---|---|
| `disk=NAME` | Boot disk (drive 1) in `/mac`. Default: `custom_753.dsk`, else `OS_750_boot.dsk` |
| `disk2=NAME` | Second disk (drive 2), LC II only |
| `mouse=25` | LC II: USB mouse speed in percent (default `25`). The movement goes through the Mac's own tracking, so the Mouse control panel also changes the speed; raise this for a slow-feeling mouse, lower it for a fast one |
| `sound=0` | LC II: switch the Apple Sound Chip off (default `1`; the Mac then sees no sound hardware) |
| `log=1` | Write a diagnostic log to `/mac/log.txt` every 30 s (default `0`: errors only, saved on a crash) |

A full System 7.5.3 needs the LC II (8 MB); the Classic has 2 MB. Big images can
be split into a bootable System disk and data disks with
[`tools/split_disk.py`](../../tools/split_disk.py), which can also disable
extensions and control panels that slow the emulated Mac down (see
[`docs/custom-disk.md`](../../docs/custom-disk.md)).

**Sound (LC II only).** With an HDMI monitor connected at power-up the Mac
screen and its sound (Apple Sound Chip, 44.1 kHz stereo) go out over HDMI. If a
monitor with speakers stays silent, add `hdmi_drive=2` to the card's
`config.txt` (forces HDMI rather than DVI mode). The Classic has no sound.

Copying to the card from Windows: **eject it before removing it** (Windows
caches writes).

## On-screen signals

| You see | Meaning |
|---|---|
| Red screen with **1** | SD card not readable (FAT) |
| Red screen with **2** | ROM missing in `/mac` |
| Red screen with **3** | Emulator start-up failed |
| Red screen with **9** | Crash; the log was saved to `/mac/log.txt` |
| Red strip at the bottom, `FRAME-PC` | The 68k has not finished a frame for 5 s; the numbers say where it is stuck |

## Building

Needs the Arm GNU toolchain (`arm-none-eabi`, tested with 13.3) and
[Circle](https://github.com/rsta2/circle) (GPL-3.0, fetched separately, not part
of this repository). `~/tools/circle/Config.mk`:

```
RASPPI = 2
PREFIX = /path/to/arm-gnu-toolchain/bin/arm-none-eabi-
DEFINE += -DKERNEL_MAX_SIZE=0x1000000
DEFINE += -DARM_ALLOW_MULTI_CORE
DEFINE += -DNO_SDHOST
```

```bash
cd ~/tools/circle && ./makeall clean && ./makeall --nosample
(cd addon/SDCard && make) && (cd addon/fatfs && make)
cd ports/rpi/macemu && make all-machines   # kernel7.img + kernel7-lc2.img
```

`NO_SDHOST` selects Circle's EMMC SD driver: the default SDHOST driver hung on
reads while USB devices were active. `KERNEL_MAX_SIZE` makes room for the
emulator's static tables; `ARM_ALLOW_MULTI_CORE` enables cores 1–3.

**Licence note:** the kernels link Circle, so the built `kernel7*.img` files are
GPL-3.0; the sources in this folder stay MIT. Never publish a card image or a
kernel together with Apple's ROMs or system software.
