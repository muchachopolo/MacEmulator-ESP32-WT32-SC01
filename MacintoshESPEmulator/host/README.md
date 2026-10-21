# Host harness (x86 Linux / WSL)

Compiles the ESP32 emulator core (`main/mac_*.c` + `components/musashi`)
**unmodified** with gcc and runs it on the PC, so boot bugs can be iterated
in about a second instead of a flash cycle. Nothing in `main/` or
`components/` is edited. ESP-IDF APIs are replaced by small headers in
`host/shim/`, which come first on the include path.

| File | Purpose |
|------|---------|
| `shim/esp_log.h` | `ESP_LOGE/W/I` print `L (ms) tag: msg` to stderr or the `--log` file. D/V are compiled out. |
| `shim/esp_attr.h` | `IRAM_ATTR`, `EXT_RAM_BSS_ATTR` and the other placement attributes become empty. |
| `shim/esp_heap_caps.h` | `heap_caps_*` call `malloc`/`calloc`. Free size is reported as 4 MB. |
| `shim/esp_timer.h`, `shim/esp_cpu.h` | Both read `CLOCK_MONOTONIC`. The cycle count is a 32-bit counter at a nominal 240 MHz, so PROFILE lines still make sense. |
| `shim/freertos/*.h` | `taskYIELD()` maps to `host_frame_hook()` (see below). `xPortGetCoreID()` is 1. `vTaskDelay` does nothing. |
| `display_stub.c` | `display_draw` captures strips into a 480x320 RGB565 buffer. `display_get_touch` plays back the `--tap` / `--swipe` scripts. |
| `rom_blobs.S` | Uses `.incbin` to embed the ROM and disk under the same `_binary_Color_Classic_ROM_*` and `_binary_OS_750_boot_dsk_*` symbols that EMBED_FILES creates. |
| `host_main.c` | Handles the CLI and per-frame hook, writes PNGs with no dependencies, dumps RAM and reports speed. |

The compile definitions match `main/CMakeLists.txt`: `MAC_ROM_LC_II=1`
(Color-Classic.ROM) and `HAS_DISK_IMAGE=1`. The build also adds
`HOST_BUILD=1`, which the core does not use yet. To build the Classic ROM
instead, use `make MAC_ROM_LC_II=0`.

## Frame pacing

`mac_emu_run()` is the firmware's endless loop. Its frames are cycle-based
(`MAC_CYCLES_PER_VBLANK` per frame). Wall-clock time from
`esp_timer_get_time` is used only for the touch-poll rate and the
SPEED/PROFILE reports, so emulated behaviour does not depend on host speed.

The loop calls `taskYIELD()` exactly once at the end of each frame, and the
shim routes that call to `host_frame_hook()`. The hook counts frames, takes
screenshots and calls `exit()` once `--frames` is reached. An `atexit`
handler prints the speed and writes `--dump-ram`.

`mac_video_task` is never started. Instead, `mac_video_render()` is called
directly from the hook on the same thread.

## Build and run

```bash
cd /mnt/e/Personal/Macintosh/MacintoshESPEmulator/host
make -j8                      # -> build/macemu   (ROM dir: ../../ROM, override ROM_DIR=...)
make run                      # 3000 frames, shots every 300, out/emu.log, out/ram.bin

./build/macemu --frames 3000 --shot-every 300 --out out \
               --dump-ram out/ram.bin --log out/emu.log
./build/macemu --frames 3000 --cpu 68030 --out out030 --log out030/emu.log
```

Options:

- `--frames N`: stop after N emulated frames. By default it runs until you press Ctrl-C, which still triggers the RAM dump.
- `--shot-every K`: every K frames, write two images. The default is 300, and 0 turns screenshots off.
  - `out/frame_NNNNN.png` is the 480x320 LCD image, made by the real `mac_video_render()`.
  - `out/scrn_NNNNN.png` is the raw 512x342 1-bit view of Mac RAM at ScrnBase (`$0824`), using the same fallback and masking as the renderer.
- `--out DIR`: the screenshot directory. The default is `out`.
- `--dump-ram FILE`: write the 2 MB Mac RAM image to FILE at exit.
- `--log FILE`: write the emulator log to FILE instead of stderr. Host `H` lines always go to stderr.
- `--stop-pc LO-HI` (TRACE=1 only): exit as soon as the 24-bit PC enters the hex range LO-HI (or a single address), so `trace_ring.txt` holds the 4096 instructions leading there.
- `--cpu 68000|68020|68030`: after `mac_emu_init()`, set the CPU type and pulse reset again. Use this to try a CPU type without editing `mac_emulator.c`.
- `--tap F`: hold the touch panel at a fixed point from frame F for 300 frames (a tap; repeatable).
- `--swipe F:dx:dy[:N]`: hold the touch from frame F for N frames (default 2000), moving it so the cursor travels exactly (dx, dy) screen pixels (both builds position the cursor 1:1, no acceleration). The point rests 400 frames at each end so the first and last polls see the exact endpoints. A `--tap` that ends a few hundred frames before a swipe makes it a tap-then-drag (button held). Leave a gap of ~200 frames between gestures: the emulator polls touch every 16 ms of wall clock (about 100 host frames), so shorter gaps can merge two gestures. A touch that starts within 400 ms (~4000 host frames) after a tap is a tap-then-drag, so leave >5000 frames after a tap that should stay a plain click. Example (LC II/Color Classic, cursor starts at (15,15); opens View and picks "by Name", then selects the Trash): `--swipe 21000:115:-6:2000 --tap 23500 --swipe 24100:0:56:3000 --swipe 29000:310:33:2500 --tap 32000 --frames 36000`. Touch is sampled inline (`TOUCH_SAMPLER_TASK=0`, `main/mac_touch.c`); the ESP32 uses a sampler task.
- `--watch ADDR` (TRACE=1): print the PC each time RAM byte ADDR (hex) changes (from `--stop-from` on).

Every run also prints `H host: ... EVQ-BAD` if the event queue head/tail ($014C/$0150) points into $00-$FF, and `H host: F... drive 1 diskInPlace $xx -> $yy` when the floppy's DrvSts changes (negative values = ejected).

`make M32=1` builds a 32-bit binary, which matches the ESP32's 32-bit `long`
and pointer sizes. It needs `gcc-multilib`.

Useful log searches:

```bash
grep -nE 'F100:|BOOT3|BB PC|ZEROIN|FIRST-BAD|CRASH|SPEED' out/emu.log
```

## Known differences from the board

- Every run is single-threaded and deterministic. Video renders between frames, never in the middle of one.
- Touch input exists only as `--tap` / `--swipe` scripts. The host runs about 100x real time, so the Mac's double-click time (in ticks) cannot be met by two scripted taps.
- Mac RAM is ordinary `malloc` memory, not PSRAM.
- Four `-Wformat` warnings appear in `mac_emulator.c:2296` because `%X` is used with `unsigned long`. They are harmless on x86-64.
