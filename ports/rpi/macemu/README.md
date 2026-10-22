# The emulator on the Raspberry Pi 2 (`macemu`)

The unmodified emulator core (`MacintoshESPEmulator/main/*.c` + Musashi) built
bare-metal with Circle for the Pi 2 + 3.5" SPI touch display. User-level setup
(SD card, settings, building Circle): [`../README.md`](../README.md). How it
works: [`docs/learn/09-raspberry-pi-port.md`](../../../docs/learn/09-raspberry-pi-port.md).

| Core | Job |
|---|---|
| 0 | the 68k (`mac_emu_run()`), interrupts, USB; its frame hook copies the Mac's disk writes into the drive-1 image (RAM) and delivers USB input |
| 1 | `mac_video_render()` → ILI9486 (changed 20-line strips, 17.9 MHz SPI) and the XPT2046 touch sampler (16 ms, also between strips); only core 1 uses SPI0 |
| 2 | SD worker: answers core 0's load requests, writes dirty blocks back 1 s after the last write, saves the log (`log=1`), prefetches the rest of the disk images |
| 3 | idle |

| Piece | File |
|---|---|
| Kernel: start-up, SD mount, ROM/config/disk loading, drive-1 write sync, error screens | `kernel.cpp` |
| ESP-IDF API shims (timer, log, heap, FreeRTOS bits) | `shim/` → `pi_platform.h` |
| Platform services, frame hook, `display.h` API | `platform.cpp` |
| Disk images in RAM: on-demand 1 MB chunks, prefetch, one dirty byte per block | `diskimage.cpp` |
| SD worker (core 2) | `sdworker.cpp` |
| Renderer + touch (core 1), freeze indicator | `videocore.cpp` |
| USB keyboard (HID usage → ADB code) and mouse | `usbinput.cpp` |
| Log tee: HDMI/serial + RAM buffer → `/mac/log.txt` | `logfile.cpp` |
| newlib stubs (`_sbrk`, `_write` → log, `%le` `sscanf`) | `syscalls.c` |

Core switches set by the Makefile: `HOST_BUILD=1` (non-ESP-IDF paths:
native-order RGB565, inline touch polling, no FreeRTOS), `MAC_FRAME_THROTTLE=1`,
`MAC_BOOT_TURBO=1`, `MAC_EXTERNAL_IMAGES=1` (ROM and disk handed over at run
time), `MAC_KEYBOARD=1`, `MAC_DRIVE2=1`; LC II: `MAC_RAM_SIZE=0x800000`,
`MAC_V8_CYCLE_SCALE=1`. The ESP32 build keeps all of them at their defaults.

**Disks.** Drive 1 goes through the core's copy-on-write overlay
(`mac_disk_overlay.c`, pool = whole image); its base image is a `CDiskImage`
that the overlay fills on demand through `disk_ov_fill`. Every 250 ms the frame
hook copies changed blocks into the image and marks them dirty. Drive 2
(`mac_v8disk.c`, `MAC_DRIVE2`) reads and writes its `CDiskImage` directly.
**FatFs is used by one core only**: core 0 before `CSDWorker::Start()`, core 2
afterwards. Every read goes through 64 KB pieces and a static bounce buffer
(large direct reads into heap buffers hung in Circle's SDHOST driver).

```bash
make                     # Mac Classic -> kernel7.img     (Classic.ROM)
make MAC_ROM_LC_II=1     # LC II       -> kernel7-lc2.img (LC-II.ROM)
make all-machines        # both (objects in obj-classic/ and obj-lc2/)
make clean-emu           # remove both
```
