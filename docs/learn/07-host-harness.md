# 7. The PC host harness — a hands-on tutorial

The host harness compiles the **same emulator core** that runs on the ESP32
(`main/mac_*.c` + Musashi) for your PC, swaps the ESP-IDF APIs for tiny shims,
and runs it single-threaded and **deterministically**. A boot to the Finder
takes about a second instead of minutes on the board, and every run with the
same inputs gives byte-identical results. Almost every bug in this project was
found here first; the board is used to confirm fixes and to measure speed.

This chapter is a practical walkthrough. The full option reference and the
internals (shims, frame hook, profiler fields) are in
[`MacintoshESPEmulator/host/README.md`](../../MacintoshESPEmulator/host/README.md).

```
            ESP32 firmware                         Host harness (PC)
   ┌──────────────────────────────┐      ┌──────────────────────────────┐
   │ main/mac_*.c  + Musashi      │ ───► │ the same files, unmodified    │
   │ ESP-IDF: esp_log, heap_caps, │      │ host/shim/*.h replace them    │
   │ esp_timer, FreeRTOS, esp_lcd │      │ display_stub.c: LCD → PNG,    │
   │ core 0 video, core 1 CPU     │      │   touch ← --tap/--swipe script│
   └──────────────────────────────┘      │ host_main.c: frame hook, CLI  │
                                         └──────────────────────────────┘
```

## 1. Setup

- Linux or **WSL** with `gcc` and `make` (on Windows: open a WSL shell).
- The ROMs and disk images in `ROM/` (see the main README and
  [custom-disk.md](../custom-disk.md)).

```bash
cd MacintoshESPEmulator/host
make -j8                 # builds build/macemu (LC II + OS_750_boot.dsk by default)
make run                 # 3000 frames, screenshots every 300 frames → out/
```

`make run` prints a speed line at the end, e.g. `H host: 3000 frames in 0.8 s`,
and leaves `out/frame_*.png` (what the LCD would show) and `out/scrn_*.png` (raw
Mac framebuffer).

**Build variants** — use a separate `BUILD=` directory per variant (make doesn't
track variables):

| Machine / disk | Command |
|---|---|
| LC II + floppy (host default) | `make BUILD=build_lc -j8` |
| Mac Classic | `make BUILD=build_cl MAC_ROM_LC_II=0 -j8` |
| Mac Classic + custom disk (board default) | `make BUILD=build_clc MAC_ROM_LC_II=0 DISK_NAME=custom_753.dsk -j8` |
| Color Classic (reference) | `make BUILD=build_cc MAC_V8_CUDA=1 -j8` |
| A switch off, e.g. idle fast-forward | `make BUILD=build_noff EXTRA_DEFS="-DMAC_IDLE_FF=0" -j8` |
| With tracing/profiling | add `TRACE=1` (and its own `BUILD=` dir) |

## 2. Boot and look at the screen

```bash
./build_clc/macemu --frames 20000 --shot-every 2000 --out shots --log shots/emu.log
grep -E 'CurApName|CRASH|SPEED|IDLE-FF' shots/emu.log
```

- Open `shots/frame_20000.png`: that is exactly what the board's LCD would show.
- `H host: F… CurApName "Finder"` marks the frame where the Finder launched —
  your boot-time metric (divide by 60 for emulated seconds).
- `CRASH`, `FIRST-BAD-PC`, `EVQ-BAD` lines point at trouble (see section 5).

## 3. Script touch input

The harness has no touch panel; you script gestures instead:

- `--tap F` — finger down at frame F for 300 frames (a click).
- `--swipe F:dx:dy[:N]` — move the cursor exactly (dx, dy) pixels over N frames
  (the cursor maps 1:1, no acceleration).
- A `--tap` ending a few hundred frames before a `--swipe` = **tap-then-drag**
  (button held: menus, window drags).
- Always add `--vclock 120`: it makes the emulator's clock count frames instead
  of wall time, so gesture timing doesn't depend on how fast your PC is.

Example (LC II, cursor starts at 15,15): open the View menu, choose "by Name",
then click the Trash:

```bash
./build_lc/macemu --vclock 120 --frames 36000 --shot-every 1000 --out g \
  --swipe 21000:115:-6:2000 --tap 23500 --swipe 24100:0:56:3000 \
  --swipe 29000:310:33:2500 --tap 32000
```

Find coordinates by looking at screenshots; keep >5000 frames between a plain
click and the next touch (otherwise it becomes a tap-then-drag). Double-clicks
can't be scripted (the host runs ~100× faster than the Mac's double-click time) —
use File → Open via tap-then-drag instead. Note: on the Classic the internal
mouse starts at the screen centre, so its swipes are relative to (256,171).

## 4. Inspect memory and saved files

```bash
./build_clc/macemu --frames 20000 --dump-ram ram.bin --disk-out disk_after.dsk
```

- `ram.bin` is the 2 MB Mac RAM: read low-memory globals with any hex tool
  (e.g. `xxd -s 0x0910 -l 32 ram.bin` = current app name) or disassemble code
  from it with `tools/dis68k.py`.
- `disk_after.dsk` is the disk as the Mac left it (flash image + PSRAM write
  overlay) — list it with Python `machfs` to see what was saved.

## 5. Chase a hang or crash

Build with tracing, then stop right where things go wrong:

```bash
make BUILD=build_tr TRACE=1 -j8
./build_tr/macemu --frames 20000 --stop-pc 2000CC        # exit when PC hits $2000CC
less trace_ring.txt                                       # the last 4096 instructions
```

- `--stop-pc LO-HI` accepts a range; `--stop-from F` arms it only from frame F
  (skip the boot).
- `--trace N` writes the first N instructions to `trace.txt` (boot problems).
- `--watch ADDR` prints the PC every time RAM byte ADDR changes — "who
  overwrote this global?" in one run.
- Typical board symptom → host recipe: take the frame number and PC from the
  board's serial log, replay the same gestures with `--vclock 120`, and stop
  just before that frame.

## 6. Find where the time goes (profiler)

```bash
make BUILD=build_prof TRACE=1 -j8
./build_prof/macemu --vclock 120 --frames 20000 --shot-every 0 \
    --profile-from 12000 --profile-to 20000 --profile-out idle.txt
```

The report ranks **A-line traps** (inclusive/self), **code regions** (4 KB
pages with their hottest PCs) and **interrupt share**. This is how we learned
that the idle Finder spends ~90% of its time busy-waiting in WaitNextEvent and
that drawing traps barely matter ([chapter 4](04-performance.md)). Combine it
with gesture scripts to profile menus, window drags or app launches.

## 7. Prove a change is safe (A/B, byte-identical)

Because runs are deterministic, "did my change alter behaviour?" has an exact
answer:

```bash
make BUILD=build_a EXTRA_DEFS="-DMY_SWITCH=0" -j8
make BUILD=build_b EXTRA_DEFS="-DMY_SWITCH=1" -j8
for v in a b; do ./build_$v/macemu --vclock 120 --frames 20000 \
    --shot-every 5000 --out out_$v --dump-ram out_$v/ram.bin >/dev/null 2>&1; done
md5sum out_a/*.png out_a/ram.bin out_b/*.png out_b/ram.bin
```

Identical hashes = identical emulation. This is how the native trap dispatcher
and the ROM-loop accelerator were verified; for changes that intentionally
alter timing (e.g. the native decompressor) compare the end state and add
built-in self-checks instead.

## 8. What the harness can't tell you

| Question | Where to answer it |
|---|---|
| Is it correct? Where does the 68k time go? | Host harness |
| How fast is it on the ESP32? (cache/IRAM/PSRAM effects) | **Board** — e.g. inlining RAM access was faster on the PC but ~16% slower on the ESP32 |
| Does touch *feel* right? Double-click timing? | Board |
| Two-core timing, SPI/LCD, I²C touch, watchdog | Board |

Workflow that worked best: reproduce on the host → fix → verify on the host
(screenshots, byte-identical A/B) → flash once → measure on the board.
