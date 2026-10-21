# Host harness (x86 Linux / WSL)

> New to the harness? Start with the hands-on tutorial:
> [docs/learn/07-host-harness.md](../../docs/learn/07-host-harness.md). This file is the reference.

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
| `shim/esp_timer.h`, `shim/esp_cpu.h` | Both read `CLOCK_MONOTONIC` (`esp_timer` returns the virtual clock instead with `--vclock`). The cycle count is a 32-bit counter at a nominal 240 MHz, so PROFILE lines still make sense. |
| `shim/freertos/*.h` | `taskYIELD()` maps to `host_frame_hook()` (see below). `xPortGetCoreID()` is 1. `vTaskDelay` does nothing. |
| `display_stub.c` | `display_draw` captures strips into a 480x320 RGB565 buffer. `display_get_touch` plays back the `--tap` / `--swipe` scripts. |
| `rom_blobs.S` | Uses `.incbin` to embed the ROM and disk under the same `_binary_<ROM>_*` and `_binary_<DISK_NAME>_*` symbols that EMBED_FILES creates. |
| `host_main.c` | Handles the CLI and per-frame hook, writes PNGs with no dependencies, dumps RAM and reports speed. |
| `host_prof.c` | Instruction profiler (`make TRACE=1`, `--profile-*`): A-line trap, code-region and interrupt attribution. |

`DISK_NAME=file.dsk` (default `OS_750_boot.dsk`) selects the disk image in the ROM directory, like `DISK_NAME` in `main/CMakeLists.txt`; use a separate `BUILD=` dir per image (objects do not track make variables).

The compile definitions mirror `main/CMakeLists.txt`: by default
`MAC_ROM_LC_II=1`, `MAC_V8_CUDA=0` (LC-II.ROM) and `HAS_DISK_IMAGE=1` when the
disk file exists. `make MAC_ROM_LC_II=0` builds the Classic (Classic.ROM),
`make MAC_V8_CUDA=1` the Color Classic (Color-Classic.ROM). The build also adds
`HOST_BUILD=1`, which selects host defaults (inline touch sampling, no frame
throttle, no RGB565 byte swap).

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
- `--disk-rw FILE`: raise the write-overlay pool to the whole image (so it never fills) and write the image as modified (flash image + overlay) to FILE at exit. Used by `tools/build_disk.py --prep` to let the Finder build the desktop database. Needs `DISK_WRITABLE=1` (default).
- `--disk-out FILE`: write the image as modified during the run (flash + PSRAM overlay, default pool) to FILE at exit, e.g. to inspect what the Mac saved. The disk is writable by default (`main/mac_disk_overlay.c`: block copy-on-write overlay in PSRAM, lost at power-off; `make EXTRA_DEFS=-DDISK_WRITABLE=0` restores the read-only volume, `-DDISK_OVERLAY_KB=N` sets the pool, default 384 Classic / 256 LC II). The emulator logs `diskov:` usage lines (blocks used / pool) on writes, at Classic shutdown and at exit.
- `--stop-pc LO-HI` (TRACE=1 only): exit as soon as the 24-bit PC enters the hex range LO-HI (or a single address), so `trace_ring.txt` holds the 4096 instructions leading there.
- `--cpu 68000|68020|68030`: after `mac_emu_init()`, set the CPU type and pulse reset again. Use this to try a CPU type without editing `mac_emulator.c`.
- `--tap F`: hold the touch panel at a fixed point from frame F for 300 frames (a tap; repeatable).
- `--swipe F:dx:dy[:N]`: hold the touch from frame F for N frames (default 2000), moving it so the cursor travels exactly (dx, dy) screen pixels (both builds position the cursor 1:1, no acceleration). The point rests 400 frames at each end so the first and last polls see the exact endpoints. A `--tap` that ends a few hundred frames before a swipe makes it a tap-then-drag (button held). Leave a gap of ~200 frames between gestures: the emulator polls touch every 16 ms of wall clock (about 100 host frames), so shorter gaps can merge two gestures. A touch that starts within 400 ms (~4000 host frames) after a tap is a tap-then-drag, so leave >5000 frames after a tap that should stay a plain click. Example (LC II/Color Classic, cursor starts at (15,15); opens View and picks "by Name", then selects the Trash): `--swipe 21000:115:-6:2000 --tap 23500 --swipe 24100:0:56:3000 --swipe 29000:310:33:2500 --tap 32000 --frames 36000`. Touch is sampled inline (`TOUCH_SAMPLER_TASK=0`, `main/mac_touch.c`); the ESP32 uses a sampler task.
- `--watch ADDR` (TRACE=1): print the PC each time RAM byte ADDR (hex) changes (from `--stop-from` on).
- `--vclock US`: virtual clock. `esp_timer_get_time()` returns `frames * US` microseconds instead of wall time, so touch polling and tap/drag classification no longer depend on host speed (the profiler slows the host 4-5x, which would otherwise turn taps into holds). `--vclock 120` matches the default build's ~8300 frames/s, so gesture scripts written for real time keep working. The speed line at exit still uses the real clock.
- `--profile-from F`, `--profile-to T`, `--profile-out FILE`, `--profile-top N` (TRACE=1): profile the instructions executed in frames [F, T) and print a report at exit (stderr and FILE). Any of these options turns the profiler on; N is the number of rows per table (default 25).

## Profiler (`host_prof.c`)

`host_trace_hook()` calls `prof_instr(pc)` before every instruction. It reads the opcode straight from the RAM array or the embedded ROM blob and never calls `m68k_read_memory_*`, so the emulation is unchanged (only touch timing depends on host speed, hence `--vclock`). The report contains:

- Instructions and Musashi cycles per frame. The cycles of an instruction are the drop in `m68ki_remaining_cycles` up to the next hook, exception processing included.
- **Context**: share of instructions inside interrupt handlers (from the PC reaching an autovector target at VBR+$64..$7C until SP rises above the entry SP, i.e. the RTE), per level; share run with the SR interrupt mask >= 1; share outside any trap.
- **A-line traps**: an `$Axxx` opcode pushes {trap, PC+2, SP}. The entry pops when the PC returns to PC+2 with SP at or above the saved SP, or as soon as SP rises above it (autopop, unwinds). *Inclusive* counts every distinct trap on the stack (nested traps count in each); *self* counts only the innermost one. Traps called inside an interrupt are charged to the interrupt, not the trap it interrupted. OS traps are keyed by number (flag bits dropped), toolbox traps without the autopop bit. Each row shows the raw trap word, the current trap-table entry ($400 OS / $E00 toolbox) and the top call sites. Unknown names print as `?`.
- **Code regions**: 4 KB pages by ROM address (ROM file offset + $A00000 on V8, + $400000 on the Classic) or RAM address, with the three hottest PCs of each page. `tools/dis68k.py` can disassemble those PCs from the ROM or a `--dump-ram` image.

```bash
make BUILD=build_prof TRACE=1 -j16
./build_prof/macemu --vclock 120 --frames 20000 --shot-every 0 \
    --profile-from 12000 --profile-to 20000 --profile-out prof_idle.txt   # Finder idle
```

The hook makes the host 4-5x slower (LC II ~1800 frames/s, Classic ~3500).

Every run also prints `H host: ... EVQ-BAD` if the event queue head/tail ($014C/$0150) points into $00-$FF, and `H host: F... drive 1 diskInPlace $xx -> $yy` when the floppy's DrvSts changes (negative values = ejected), and `H host: F... CurApName "..."` when the current application changes (`"Finder"` = boot-to-Finder frame). Every 3000 frames it prints `H host: F... Time $xxxxxxxx` (the Mac clock at $20C; it should advance by 50 = 3000/60). The Classic build posts clicks into the OS's own event buffer (`CLASSIC_MOUSE_POSTEVENT=1`, default), so EVQ-BAD should never appear; with `EXTRA_DEFS=-DCLASSIC_MOUSE_POSTEVENT=0` the old fixed EvQEl buffers at $C0/$D8 trigger it by design after a click. EVQ-BAD prints at most 10 times; CurApName changes at most 200 times (a foreground app that is not background-capable makes it alternate with the Finder's background time).

`make EXTRA_DEFS="-DMAC_IDLE_FF=0 -DMAC_V8_CYCLE_SCALE=1"` adds compile definitions, e.g. to A/B the idle fast-forward and the V8 cycle scale (see `main/mac_emulator.h`; with both off the core behaves exactly as before them). The real-time frame throttle (`MAC_FRAME_THROTTLE`) defaults to off on the host. The emulator logs an `IDLE-FF:` line with each `SPEED` report (share of CPU cycles skipped, number of fast-forwards per detector).

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
