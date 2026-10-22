# The emulator on the Raspberry Pi 2 (phases R3 and R5)

The unmodified emulator core (`MacintoshESPEmulator/main/*.c` + Musashi) built
bare-metal with Circle for the Pi 2 + AC-TFT35, single core. Phase R3 of
[`spec/raspberry-pi-port.md`](../../../spec/raspberry-pi-port.md).

| Piece | File |
|---|---|
| ESP-IDF API shims (timer, log, heap, FreeRTOS bits) | `shim/` → `pi_platform.h` |
| Platform services + `display.h` API (ILI9486 at 17.9 MHz, XPT2046 with the R2 calibration) | `platform.cpp` |
| newlib stubs (`_sbrk`, `_write` → log, `%le` `sscanf`) | `syscalls.c` |
| ROM + disk loaded from the SD card, disk write-back (R5) | `kernel.cpp` |

The core is compiled with `HOST_BUILD=1` (non-ESP-IDF paths: native-order RGB565,
inline touch polling, no FreeRTOS) plus `MAC_FRAME_THROTTLE=1` and
`MAC_EXTERNAL_IMAGES=1` (ROM and disk handed over at run time). The 68k runs in
`mac_emu_run()`; the per-frame hook renders changed strips at most every 33 ms
and checks the CPU temperature throttle. The ARM runs at its maximum clock.

## SD card

```
/bootcode.bin /start.elf /fixup.dat /config.txt /kernel7.img   (as for R1)
/mac/Classic.ROM          (LC-II.ROM for the LC II build)
/mac/custom_753.dsk       (any HFS disk image up to the floppy-path limit)
/mac/config.txt           optional: disk=<file name in /mac>
```

Without `disk=` the Mac boots `custom_753.dsk`, else `OS_750_boot.dsk`, else
shows the "?" floppy. The whole image is loaded into RAM; blocks the Mac writes
are written back to the file about one second after the Mac stops writing (at
most every 10 s while it keeps writing) and before a restart, so saved files
survive power-off. A red screen with a number means: 1 = no SD card/FAT, 2 = ROM
missing, 3 = emulator init failed.

## Build

Circle must be built with a larger kernel limit (the emulator's static tables):
add `DEFINE += -DKERNEL_MAX_SIZE=0x1000000` to `~/tools/circle/Config.mk`,
rebuild Circle (`./makeall clean && ./makeall --nosample`) and build the
`addon/SDCard` and `addon/fatfs` libraries (`make` in each).

```bash
make                     # Mac Classic (Classic.ROM)
make MAC_ROM_LC_II=1     # LC II (LC-II.ROM; run `make clean-emu` when switching)
```
