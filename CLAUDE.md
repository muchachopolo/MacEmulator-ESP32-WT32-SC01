# Macintosh emulator for ESP32 (WT32-SC01) — contributor and agent guide

A 68k Macintosh emulator (Musashi CPU core) for the WT32-SC01 v3.4 board
(ESP32-WROVER-B, ST7796S 480×320 SPI LCD, FT5x06 touch). User-facing overview:
`README.md`. **The ordered plan and current state live in
`spec/next-steps-plan.md`**; history and measurements in
`spec/performance-roadmap.md`. Detailed docs: `docs/`.

## Ground rules

- **Host first.** Debug emulation logic on the x86 host harness
  (`MacintoshESPEmulator/host/`); flash the board only to confirm a fix or to
  measure speed. Only the main session flashes the board.
- **Never commit ROMs or disk images** (Apple copyright). `ROM/` is git-ignored;
  never publish built firmware (it embeds the ROM and disk).
- **Committed defaults** (user decision 2026-10-21): `ROM_NAME "Classic.ROM"`
  and `DISK_NAME "custom_753.dsk"` in `main/CMakeLists.txt`; CMake falls back
  to `OS_750_boot.dsk` when the custom disk hasn't been built. The disk image
  itself and its recipe are local only (`ROM/`, git-ignored).
- **No `Co-Authored-By` lines** in commit messages.
- Measure speed as ESP32 exec cycles per 300 frames or wall-clock per task
  (boot-to-Finder seconds), never real-time % across different machines.
- Every native replacement must be byte-identical (RAM dumps + screenshots on the
  host) with its switch ON vs OFF, and gets its own switch.
- Decode `4E FA` as `JMP d16(PC)`.
- Parallel agents: separate host `BUILD=` dirs and scratch dirs, clear file ownership.

## Repository layout

Repo root: `E:\Personal\Macintosh` (WSL: `/mnt/e/Personal/Macintosh`).

```
MacintoshESPEmulator/        ESP-IDF project
  main/                      emulator (C) + main.cpp (entry point)
  components/musashi/        Musashi 68k core (copied, patched) + mac_native.c
  components/display/        esp_lcd ST7796 + FT5x06 driver
  host/                      x86 host harness (Makefile, shims, profiler)
  build.bat                  ESP-IDF v6.1 wrapper (run via cmd.exe)
  partitions.csv             4 MB flash: one 0x3F0000 factory app partition
ROM/                         ROMs + disk images (git-ignored, user-supplied)
tools/                       build_disk.py, dis68k.py, romcheck.py, serial_watch.py, HFS parsers
board_test/                  hardware diagnostic project
docs/                        architecture, build, hardware, memory map, CPU, peripherals, display
spec/                        next-steps-plan.md (plan), performance-roadmap.md (history)
```

## Machines and build variables

One codebase, two machine targets, picked in `main/CMakeLists.txt`:

| `ROM_NAME` | Machine | Derived defines |
|---|---|---|
| `Classic.ROM` (committed default) | Mac Classic: 68000, 1-bit | `MAC_ROM_LC_II=0` |
| `LC-II.ROM` | LC II: 68030, V8 chipset, Egret, colour | `MAC_ROM_LC_II=1`, `MAC_V8_CUDA=0` |
| `Color-Classic.ROM` | Color Classic (reference only, 1 MB ROM, Cuda) | `MAC_ROM_LC_II=1`, `MAC_V8_CUDA=1` |

`DISK_NAME` (default `custom_753.dsk`, falling back to `OS_750_boot.dsk` if
absent) selects the disk image in `ROM/`; `DISK_SYM` is its C identifier.
`custom_753.dsk` (2880K) comes from `tools/build_disk.py` — user guide: `docs/custom-disk.md` (keep your build command locally in `ROM/`; host harness default stays
LC II + `OS_750_boot.dsk`, see host/README.md). ROM checksums: Classic `A49F9914`, LC II `35C28F5F`,
Color Classic `ECD99DC0`.

The host Makefile mirrors this: `make MAC_ROM_LC_II=0` (Classic),
`make MAC_V8_CUDA=1` (Color Classic), `make DISK_NAME=custom_753.dsk BUILD=build_cd`,
`make EXTRA_DEFS="-DSWITCH=0"`. Objects do not track make variables: use a
separate `BUILD=` dir per configuration.

## Building

ESP-IDF **v6.1** at `C:\esp\v6.1\esp-idf`, tools at `C:\Espressif\tools`.
ESP-IDF rejects MSYS/MinGW shells: always go through `build.bat` via `cmd.exe`.

```bash
cmd.exe //C "E:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat build"
cmd.exe //C "E:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat -p COM3 flash"
cmd.exe //C "E:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat size"
cmd.exe //C "E:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat fullclean"   # then set-target esp32
```

Serial port **COM3** (CH340). `tools/serial_watch.py` logs it to
`tools/serial_watch.log`; kill stale watcher processes before flashing (they
hold the port).

Musashi's generated `m68kops.c/.h` are **committed** (so clones build); after editing `m68k_in.c` regenerate with
`gcc -o m68kmake m68kmake.c && ./m68kmake . m68k_in.c` in
`components/musashi/`. `m68k_in.c` carries `EXT_RAM_BSS_ATTR` on the two big
tables (without it `.dram0.bss` overflows by ~434 KB). `m68kmake` ignores
`m68kconf.h`, so a config flip needs no regeneration.

### Host harness

```bash
cd MacintoshESPEmulator/host && make -j16 && make run         # 3000 frames, PNGs in out/
make TRACE=1 -j16 && ./build_trace/macemu --frames 600 --trace 100000 --log emu.log
make BUILD=build_prof TRACE=1 -j16 && ./build_prof/macemu --vclock 120 --profile-from 12000 --profile-to 20000
```

Options (`--tap`, `--swipe`, `--stop-pc`, `--watch`, `--disk-rw`, `--disk-out`,
`--cpu`, profiler) and known differences from the board: `host/README.md`.
Useful log markers: `CurApName "Finder"` = boot-to-Finder frame, `SPEED`,
`PROFILE`, `IDLE-FF:`, `diskov:`.

## Architecture (short)

Details: `docs/architecture-overview.md`.

- **Core 1**: `mac_emu_run()` — frame loop, 60.15 Hz, `MAC_CYCLES_PER_VBLANK`
  ≈130,560 device cycles per frame, executed in slices; device ticks (VIA,
  Egret, SWIM) between slices; frame-end touch/disk updates; idle fast-forward;
  real-time throttle.
- **Core 0**: `mac_video_task` (renderer, 20-line DMA strips, dirty-strip skip)
  and the touch sampler task.
- **Classic** (`MAC_ROM_LC_II=0`): 68000, map RAM/ROM $400000/SCSI/SCC/IWM/VIA,
  ROM patched to a native 480×320 visible screen (`CLASSIC_NATIVE_LCD`), SWIM
  ISM emulation + .Sony Device Manager intercepts serve the disk, RTC/PRAM chip
  (`mac_rtc.c`), clicks via a C mirror of the ROM's PostEvent.
- **V8 / LC II** (`MAC_ROM_LC_II=1`): 68030 with a PMMU instruction stub (no
  translation), ROM at $A00000, devices at $F00000+ (VIA1, SCC, SCSI, ASC,
  SWIM, VDAC $F24000, pseudo-VIA2 $F26000, VRAM $F40000), overlay clears on
  the first access to $A00000. Egret/Cuda HLE (`mac_egret.c`: ADB mouse, RTC,
  PRAM, 1 Hz tick), video (`mac_v8video.c`: VRAM in PSRAM, CLUT, ROM VPBlocks
  patched to 480×320), disk via a .Sony Prime hook (`mac_v8disk.c`).
- **Disk**: image embedded in flash (read-only) + PSRAM copy-on-write overlay
  (`mac_disk_overlay.c`); lost at power-off.
- **Native speed-ups** (`components/musashi/mac_native.c`): trap dispatcher,
  dcmp 3 decompressor, ROM loop accelerator.

Classic boot workarounds (memory-test D6 force, SCSI polling bypass and
boot-mode $00 reads, overlay one-shot latch, .Sony I/O intercepts and DM-spin
rescue at $4031BE, ADB completion flag, StartUpScreen BNE→BRA, UNIMP handler,
SCC RR0 Sync/Hunt, Mini vMac-style checksum/RAM-test skips) are still in
`mac_emulator.c`/`mac_memory.c`. Their history is in
`spec/performance-roadmap.md` and `docs/architecture-overview.md`; most are keyed to Classic ROM
addresses and gated off on V8.

### CRITICAL notes that still apply

- **`fast_slice` ≤ 4000 cycles** (`mac_emulator.c`, divided by
  `MAC_CYCLE_SCALE`). Larger slices make `via_tick()` miss short VIA T2
  periods and crash deterministically (tested up to 40000).
- **CrsrNew ($08CE) only on a position change**, and only once WMgrPort
  ($09DE) is valid. Writing it every frame forces cursor redraws that crash the
  resource decompressor during INIT/Finder loading. Initial tracking values
  must equal the `mouse_x/mouse_y` defaults.
- **Clicks must be posted to the event queue** (EvQHdr $014A). MBState alone
  is not enough: GetNextEvent/WaitNextEvent/ModalDialog read the queue. Post
  only while the 68k interrupt mask is 0.
- **MBState is written as a whole byte** ($00 down / $80 up): the shutdown
  screen's Restart loop waits for exactly $00.
- **VIA IRB initial value $31** (PB3=0, button down during boot) is required
  for the Classic boot.
- **V8: the overlay is not cleared by VIA ORA writes**; the ORA latch is Classic-only.
- **Mac RAM is 2 MB** (power of two, aliased via `ram_mask` on the Classic;
  V8 reads $FF above `ram_size` so the ROM sizes RAM correctly).

## Touch input

`main/mac_touch.c` (gestures, sampler) + `main/mac_emulator.c` (apply).
Details: `docs/display-pipeline.md#touch-input`.

- Swipe (> 8 px) moves the cursor 1:1, no button; tap (< 300 ms, < 8 px)
  clicks; a second touch within 400 ms of a tap is tap-then-drag; after 300 ms
  a 3 px nudge also counts as a drag. Each button state is held ≥ 2 frames.
- `TOUCH_SAMPLER_TASK=1` (ESP32): sampler task on core 0 at ~60 Hz, the loop
  calls `touch_consume()` once per frame (spinlock-guarded). Host: inline polling.
- Position: both machines write MTemp/RawMouse/Mouse ($0828/$082C/$0830,
  Point = Y.hi Y.lo X.hi X.lo) and CrsrNew (if CrsrCouple), gated on
  `mac_post_boot` + valid WMgrPort.
- Button, **Classic**: `CLASSIC_MOUSE_POSTEVENT=1` mirrors the ROM's ADB mouse
  handler ($403A54: RndSeed, MBTicks, MBState, PostEvent $403EE6) into the
  System's own SysEvtBuf; plus MBState and VIA PB3 (`via_set_mouse_button()`).
- Button, **LC II**: the emulated Egret ADB mouse carries the button (zero
  deltas); movement after a button transition waits until the transition is in
  an ADB packet.
- **Game mode** (`TOUCH_GAME_MODE`): a windowless non-Finder app in front
  (WindowList $09D6 = NULL, ~1 s score with hysteresis) or a hidden cursor →
  clicks posted at the menu bar (v=1), so the Process Manager does not switch
  the game out.
- **Game full view** (`GAME_FULL_VIEW`, Classic native only): while game mode
  holds (0.5 s in, 1 s out) and the game has drawn into the 512×342 margin
  (columns 480-511 / rows 320-341, compared with a baseline taken outside
  games), `mac_full_view` makes the renderer show the whole 512×342 scaled to
  480×320; CrsrPin $0834 becomes 0,0,342,512 and finger deltas are scaled
  ×512/480, ×342/320. Leaving restores CrsrPin and clamps the cursor.
  `GAME_FULL_SCREENBITS`: apps in `full_screen_apps[]` (`mac_memory.c`,
  matched by CurApName prefix) get the stock 512×342
  from ScrnSize/_ScrnBitMap, so a game that centres its field on screenBits is
  not cut off.
- **LC II game full view** (`V8_GAME_FULL_VIEW`): InitGraf ($A86E, seen at the
  A-line hook) from an app in `full_screen_apps[]` sets the main GDevice's
  gdRect and PixMap bounds to 512×342 (`V8_GAME_FULL_H`), so Color QuickDraw
  gives it 512×342 screenBits; the renderer scales that VRAM area to 480×320,
  CrsrPin is widened, touch deltas are scaled and game-mode clicks keep h < 480
  (the menu bar stays 480 wide). The app's ExitToShell (also taken after a
  system-error quit) or InitGraf from an unlisted app restores the 480×320
  device, CrsrPin and the 1:1 view.

## Build switches

`-D` defines (C headers) unless noted; default in brackets.

| Switch | Where | Meaning |
|---|---|---|
| `CLASSIC_NATIVE_LCD` [1] | mac_emulator.h | Classic ROM patched to a 480×320 visible screen, 1:1 render; 0 = stock 512×342 scaled |
| `V8_NATIVE_LCD` [1] | mac_v8video.h | LC II 512×384 VPBlocks patched to 480×320 |
| `GAME_FULL_VIEW` [1] | mac_emulator.h | Classic: scaled 512×342 view for full-screen games (forced 0 on LC II or with `CLASSIC_NATIVE_LCD=0`) |
| `GAME_FULL_SCREENBITS` [=`GAME_FULL_VIEW`] | mac_emulator.h | Listed games get stock 512×342 screenBits |
| `V8_GAME_FULL_VIEW` [1] | mac_v8video.h | LC II: listed games get a 512×`V8_GAME_FULL_H` main GDevice at InitGraf, shown scaled (forced 0 with `V8_NATIVE_LCD=0`) |
| `V8_GAME_FULL_H` [342] | mac_v8video.h | LC II game screen height (384 = the 12" mode; letterboxed, smaller) |
| `CLASSIC_VIA_ECLOCK` [=`CLASSIC_NATIVE_LCD`] | mac_via.c | Classic VIA timers count at the E clock (CPU/10) |
| `CLASSIC_VIA_INTERP` [1] | mac_via.c | Classic VIA timers interpolated mid-slice |
| `CLASSIC_VIA_IRQ_NOW` [1] | mac_emulator.h | Classic: drive the level-1 IRQ line on every IFR/IER change |
| `CLASSIC_RTC` [1] | mac_rtc.h | Classic RTC/PRAM chip emulation (0 = frozen clock, PRAM $FF) |
| `CLASSIC_MOUSE_POSTEVENT` [1] | mac_emulator.c | ROM-faithful PostEvent mirror (0 = legacy EvQEl buffers at $C0/$D8) |
| `TOUCH_GAME_MODE` [1] | mac_emulator.c | Game mode click routing |
| `TOUCH_SAMPLER_TASK` [1 ESP32 / 0 host] | mac_touch.h | Touch sampled by a core-0 task |
| `MAC_IDLE_FF` [1] | mac_emulator.h | Idle fast-forward (Process Manager idle loop, Ticks-wait loops) |
| `MAC_FRAME_THROTTLE` [1 ESP32 / 0 host] | mac_emulator.h | At most one frame per 16.626 ms |
| `MAC_V8_CYCLE_SCALE` [2] | mac_emulator.h | LC II: CPU gets 1/N cycles per frame, device time ×N (Classic always 1) |
| `EGRET_ONE_SEC_TICKS` [1] | mac_egret.c | Egret/Cuda 1 Hz tick packets (Mac clock advances) |
| `DISK_WRITABLE` [1] | mac_disk_overlay.h | PSRAM copy-on-write disk overlay, volume unlocked |
| `DISK_OVERLAY_KB` [384 Classic / 256 LC II] | mac_disk_overlay.h | Overlay pool size |
| `MAC_DIAG` [0] | mac_memory.h | Boot diagnostics/spin detectors on memory hot paths |
| `MAC_VIDEO_DIRTY` [1] | mac_video.c | Skip unchanged 20-line strips |
| `MAC_MEM_CALLBACK_IRAM` [1] | mac_memory.c | Musashi memory callbacks in IRAM |
| `MUSASHI_HOT_IRAM` [ON] (CMake) | components/musashi/CMakeLists.txt | `m68k_execute` + 128 hottest handlers in IRAM (`hot_iram.lf`) |
| `M68K_INLINE_RAM` [OFF] | m68kconf.h | Inline RAM data accesses (faster on x86, slower on ESP32 I-cache) |
| `M68K_INLINE_FETCH` [OFF] | m68kconf.h | Inline instruction fetches (−8% cycles when OFF on ESP32) |
| `M68K_OP_INDEX` [OFF] | m68kconf.h | 16-bit opcode index table (−3%, costs 128 KB PSRAM + 10 KB DRAM) |
| `M68K_EMULATE_BUS_ERROR` [OFF] | m68kconf.h | Bus-error machinery (nothing raises one) |
| `MAC_NATIVE_TRAPDISP` [1] | m68kconf.h | Native A-line trap dispatcher (byte-identical) |
| `MAC_NATIVE_DCMP` [1] | m68kconf.h | Native 'dcmp' 3 decompressor (output-exact; `MAC_NATIVE_DCMP_CHECK` host check) |
| `MAC_NATIVE_LOOPS` [1] | m68kconf.h | Native long ROM loops at slice start (byte-identical) |

Also fixed in `m68kconf.h`: `M68K_SEPARATE_READS` ON (lean instruction-fetch
path), 020/030 emulation ON, `M68K_EMULATE_PMMU` OFF (stub in `m68kmmu.h`).

## Resource budget (2026-10-21)

| Resource | Use |
|---|---|
| Flash app partition 0x3F0000 | Firmware ~0x3E18F0 with the 2880K custom disk (about 1.4 MB less with the 1.44 MB floppy). The disk can be ~2.89 MB at most. |
| IRAM | ~77.5 KB used, ~53 KB free (hot Musashi handlers, memory callbacks, lean fetch) |
| PSRAM (~4 MB addressable of 8 MB) | Mac RAM 2 MB; Musashi jump + cycle tables ~580 KB (`EXT_RAM_BSS_ATTR`); LC II VRAM 512 KB; disk overlay 384 KB (Classic) / 256 KB (LC II) + 2 bytes/block index; LC II declaration-ROM copy ~5 KB |
| Internal DMA RAM | 480×20×2 = 19.2 KB line buffer |

## Conventions

- C for emulation (`main/*.c`), C++ only for `main.cpp` and the display driver;
  headers have `extern "C"` guards.
- Big-endian access via the byte macros; Mac Points are vertical-first.
- Large buffers in PSRAM (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`), DMA buffers in
  internal RAM (`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`).
- Third-party code (Musashi) is copied in; keep its licence header.
- Every new behaviour change gets a switch with the old behaviour at 0 for board A/B.
- `m68kdasm.c` is not compiled (saves DRAM); use `tools/dis68k.py` or the host trace.

## Hardware facts

WT32-SC01 v3.4: ESP32-D0WD rev 1.0 at 240 MHz; flash **4 MB** (label says 16 MB),
80 MHz **QIO**; PSRAM 8 MB at 80 MHz, ~4 MB usable; ST7796S on SPI2 at 40 MHz
(MOSI 13, CLK 14, CS 15, DC 21, RST 22, BL 23), BGR element order; FT5x06 on
I2C (SDA 18, SCL 19, 0x38). PSRAM cache workaround: MEMW (DUPLDST measured
1.3% slower). Details: `docs/hardware.md`.
