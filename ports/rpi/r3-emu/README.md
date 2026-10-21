# R3 — the emulator on the Raspberry Pi 2 (bring-up)

The unmodified emulator core (`MacintoshESPEmulator/main/*.c` + Musashi) built
bare-metal with Circle for the Pi 2 + AC-TFT35, single core. Phase R3 of
[`spec/raspberry-pi-port.md`](../../../spec/raspberry-pi-port.md).

| Piece | File |
|---|---|
| ESP-IDF API shims (timer, log, heap, FreeRTOS bits) | `shim/` → `pi_platform.h` |
| Platform services + `display.h` API (ILI9486 at 17.9 MHz, XPT2046 with the R2 calibration) | `platform.cpp` |
| newlib stubs (`_sbrk`, `_write` → log, `%le` `sscanf`) | `syscalls.c` |
| ROM + disk embedded as `_binary_*` symbols (bring-up; R5 reads them from SD) | `blobs.S` |

The core is compiled with `HOST_BUILD=1` (non-ESP-IDF paths: native-order RGB565,
inline touch polling, no FreeRTOS) plus `MAC_FRAME_THROTTLE=1`. The 68k runs in
`mac_emu_run()`; the per-frame hook renders changed strips at most every 33 ms
and checks the CPU temperature throttle. The ARM runs at its maximum clock.

## Build

Circle must be built with a larger kernel limit (the image holds the ROM and the
disk): add `DEFINE += -DKERNEL_MAX_SIZE=0x1000000` to `~/tools/circle/Config.mk`
and rebuild Circle (`./makeall clean && ./makeall --nosample`).

```bash
make                     # Classic.ROM + ROM/custom_753.dsk (or OS_750_boot.dsk)
make MAC_ROM_LC_II=1     # LC II (use a separate checkout or `make clean-emu` first)
```

`kernel7.img` embeds Apple's ROM and your disk image: **never publish it**.
Copy it to the SD card prepared for R1.
