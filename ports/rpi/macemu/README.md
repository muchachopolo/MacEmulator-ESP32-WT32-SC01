# The emulator on the Raspberry Pi 2 (`macemu`)

The unmodified emulator core (`MacintoshESPEmulator/main/*.c` + Musashi) built
bare-metal with Circle for the Pi 2 + AC-TFT35: core 0 runs the 68k, core 1
draws the screen and reads touch, core 2 does all SD card access (loading
disks on demand and in the background, saving changes, the log). Phases R3, R5, R6 and R7 (multi-core) of
[`spec/raspberry-pi-port.md`](../../../spec/raspberry-pi-port.md).

| Piece | File |
|---|---|
| ESP-IDF API shims (timer, log, heap, FreeRTOS bits) | `shim/` → `pi_platform.h` |
| Platform services + `display.h` API | `platform.cpp` |
| Disk images in RAM: on-demand 1 MB chunks, background prefetch, dirty blocks | `diskimage.cpp` |
| Core 2: SD worker (load requests, write-back, prefetch, log) | `sdworker.cpp` |
| Core 1: renderer + touch sampler (ILI9486 at 17.9 MHz, XPT2046 with the R2 calibration, ~60 Hz) | `videocore.cpp` |
| newlib stubs (`_sbrk`, `_write` → log, `%le` `sscanf`) | `syscalls.c` |
| ROM + disk loaded from the SD card, disk write-back (R5) | `kernel.cpp` |

The core is compiled with `HOST_BUILD=1` (non-ESP-IDF paths: native-order RGB565,
inline touch polling, no FreeRTOS) plus `MAC_FRAME_THROTTLE=1` and
`MAC_EXTERNAL_IMAGES=1` (ROM and disk handed over at run time). The 68k runs in
`mac_emu_run()` on core 0; its per-frame hook saves the disk and checks the CPU
temperature throttle. Core 1 loops over `mac_video_render()` (changed 20-line
strips only) and samples touch every 16 ms, also between strips, like the
ESP32's core-0 tasks. Only core 1 uses SPI0. The ARM runs at its maximum clock.

## SD card

```
/bootcode.bin /start.elf /fixup.dat                (as for R1)
/config.txt               arm_64bit=0 and kernel=kernel7.img (Classic) or kernel=kernel7-lc2.img (LC II)
/kernel7.img /kernel7-lc2.img
/mac/Classic.ROM          (LC-II.ROM for the LC II build)
/mac/custom_753.dsk       (any HFS disk image up to the floppy-path limit)
/mac/config.txt           optional: disk=<file in /mac>, disk2=<file> (LC II), log=1
```

Without `disk=` the Mac boots `custom_753.dsk`, else `OS_750_boot.dsk`, else
shows the "?" floppy. The whole image is loaded into RAM; blocks the Mac writes
are written back to the file about one second after the Mac stops writing (at
most every 10 s while it keeps writing) and before a restart, so saved files
survive power-off. A red screen with a number means: 1 = no SD card/FAT, 2 = ROM
missing, 3 = emulator init failed, 9 = crash. `log=1` writes everything to
`/mac/log.txt` (every 30 s); with the default `log=0` only errors are kept and the
file is written only on a crash, since every SD write briefly pauses the Mac.

## Build

Circle must be built with a larger kernel limit (the emulator's static tables)
and multi-core support, using the EMMC SD driver (the default SDHOST driver
hung on reads while USB devices were active): add
`DEFINE += -DKERNEL_MAX_SIZE=0x1000000`, `DEFINE += -DARM_ALLOW_MULTI_CORE` and
`DEFINE += -DNO_SDHOST` to `~/tools/circle/Config.mk`,
rebuild Circle (`./makeall clean && ./makeall --nosample`) and build the
`addon/SDCard` and `addon/fatfs` libraries (`make` in each).

```bash
make                     # Mac Classic -> kernel7.img     (Classic.ROM)
make MAC_ROM_LC_II=1     # LC II       -> kernel7-lc2.img (LC-II.ROM)
make all-machines        # both (objects in obj-classic/ and obj-lc2/)
```
