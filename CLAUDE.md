# Macintosh Classic Emulator for ESP32

## Project Overview

A Macintosh Classic emulator running on the WT32-SC01-3.4 board (ESP32-WROVER-B).
Emulates a 68000 CPU via Musashi, with 512x342 monochrome display scaled to 480x320 RGB565 on an ST7796 SPI LCD with FT5x06 capacitive touch.

**Two targets** (selected by `ROM_NAME` in `main/CMakeLists.txt`):
- **Classic.ROM** (68000, mono): the working build and fallback. Last fully working commit: c9abaef. Same 512KB ROM size as LC-II.ROM. If LC II at 1-bit measures as fast as Classic, LC II replaces it (one build: 1-bit for speed, colour on demand).
- **V8 / 68030** (`MAC_ROM_LC_II=1`, colour-capable): in progress. **Build uses LC-II.ROM** (user decision 2026-10-20; `MAC_V8_CUDA=0`, Egret). Color-Classic.ROM remains selectable (`MAC_V8_CUDA=1`, Cuda) only as a reference. Done: Egret/Cuda HLE (`main/mac_egret.c`), 68030 MMU instruction stub (no translation), V8 video hardware (`main/mac_v8video.c`, VRAM $F40000). V8 floppy via a .Sony Prime hook (`main/mac_v8disk.c`), VRAM/CLUT renderer, and a native **480×320** mode patched into the ROM's video declaration blocks (1:1 on the LCD). On the host the LC II boots System 7.5.3 to the Finder. Still unverified on hardware: panel colour order, speed, and touch/mouse on V8. See `spec/performance-roadmap.md` → "Phase 2 attempt 4" and "ROM choice reassessment".

**Next work:** follow `spec/next-steps-plan.md` (ordered plan: commit/cleanup → board A/B incl. an open idle-speed regression → keyboard → native trap acceleration). Work through 2026-10-21 is committed on branch `phase2-lc2-native` (not yet merged to main).

**Current status (Classic build)**: System 7.5.3 Finder launches and runs stably with trackpad-style touch input and working mouse clicks. Full boot sequence completes (ROM self-test → boot block → System file → INITs → _InitWindows → _Launch → Finder). Speed: 39-91% real-time (peak ~91% at idle, 57-67% during boot). Touch screen works as relative trackpad (swipe to move cursor without clicking, tap to click, tap-then-drag for click-and-drag).

## Repository Layout

```
D:\Personal\Macintosh\
├── MacintoshESPEmulator/     # ESP-IDF project (the emulator)
│   ├── main/                 # Application source code
│   │   ├── mac_emulator.c    # Main emulation loop + boot bypasses
│   │   ├── mac_memory.c      # Memory map + trap intercepts + disk I/O
│   │   ├── mac_via.c         # VIA 6522 (timers, interrupts, ADB, overlay)
│   │   ├── mac_scsi.c         # NCR 5380 SCSI controller (arbitration, no devices)
│   │   ├── mac_scc.c         # SCC Z8530 stub (serial ports)
│   │   ├── mac_iwm.c         # IWM/SWIM floppy controller
│   │   ├── mac_video.c       # Framebuffer → LCD rendering (core 0)
│   │   └── main.cpp          # ESP-IDF entry point (C++ for display driver)
│   ├── components/
│   │   ├── musashi/          # Musashi 68000 CPU emulator (copied source)
│   │   └── display/          # Native esp_lcd display driver
│   ├── host/                 # x86 host harness: same core + ESP-IDF shims (fast debugging)
│   ├── build.bat             # ESP-IDF v6.1 build wrapper (must use cmd.exe)
│   ├── partitions.csv        # Custom partition table (4MB flash)
│   └── sdkconfig.defaults    # SDK configuration
├── ROM/                      # ROM and disk images (not in git, gitignored)
│   ├── Classic.ROM           # 512KB Mac Classic ROM (required)
│   ├── OS_750_boot.dsk       # 1.44MB System 7.5.3 boot floppy
│   ├── Color-Classic.ROM     # 1MB, 68030 V8-family (current V8 bring-up ROM)
│   └── LC-II.ROM             # 512KB, 68030 V8-family, complete dump (planned V8 ROM)
├── board_test/               # Hardware diagnostic project
├── docs/                     # Architecture documentation
├── tools/                    # ROM analysis & disk parsing scripts
├── *.py                      # ROM dump/trace utilities
└── *.pdf                     # Board/chip datasheets
```

## Build System

**ESP-IDF v6.0** installed via EIM at `C:\esp\v6.0\esp-idf`, tools at `C:\Espressif\tools`.

### CRITICAL: ESP-IDF v6.0 rejects MSYS/MinGW shells

You MUST build via `cmd.exe`, not bash. Use the wrapper:

```bash
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat build"
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat flash"
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat monitor"
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat size"
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat fullclean"
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat set-target esp32"
```

After `fullclean`, you must run `set-target esp32` before building.

### Host harness (debug on the PC first)

`MacintoshESPEmulator/host/` compiles the unmodified `main/mac_*.c` + Musashi for x86 (WSL gcc) against small ESP-IDF shims. 3000 frames run in under 1 s, versus minutes on the board. It writes LCD screenshots as PNG, can dump RAM, and `make TRACE=1` gives a per-instruction trace. Debug emulation logic here; flash the board only to confirm fixes and to measure speed. See `host/README.md`.

```bash
cd MacintoshESPEmulator/host && make -j16 && make run
make TRACE=1 -j16 && ./build_trace/macemu --frames 600 --trace 100000 --log emu.log
```

### Musashi Code Generation

Musashi requires a two-stage build. The generated files (`m68kops.c`, `m68kops.h`) are **gitignored** — they exist only in the working tree, and regeneration replaces them. `m68k_in.c` (tracked) carries `EXT_RAM_BSS_ATTR` on the two big tables so every regeneration places them in PSRAM; without it the link overflows `.dram0.bss` by ~434KB. `m68kmake` does not read `m68kconf.h` — generated ops are config-independent, so a conf flip alone needs no regeneration.

To regenerate (requires a host C compiler like MinGW GCC):
```bash
cd MacintoshESPEmulator/components/musashi
gcc -o m68kmake m68kmake.c
./m68kmake . m68k_in.c
```

## Coding Conventions

- **C** for emulation code (`main/*.c`), **C++** only for `main.cpp` (ESP-IDF entry point) and display driver
- All emulator headers use `extern "C"` guards for C++ compatibility
- Third-party code (Musashi) is copied into the project, not referenced externally
- Big-endian byte access via macros (`READ_BYTE`, `READ_WORD`, `READ_LONG`)
- Memory allocation: large buffers go to PSRAM via `heap_caps_malloc(MALLOC_CAP_SPIRAM)`, DMA buffers stay in internal SRAM via `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`
- Musashi large BSS tables (opcode jump table, cycle table) use `EXT_RAM_BSS_ATTR` to place them in PSRAM

## Hardware Target (verified via board diagnostic 2026-04-08)

- **Board**: WT32-SC01-3.4 (Wireless Tag)
- **SoC**: ESP32-D0WD rev v1.0 (dual-core Xtensa LX6 @ 240MHz)
- **Flash**: **4MB** (detected, despite "16MB" label on board), 80MHz **QIO** (verified 2026-10-20, +1.1% speed vs DIO)
- **PSRAM**: 8MB physical, **~4MB usable** (ESP32 virtual address space limit), 80MHz quad SPI
- **Display**: ST7796 480x320 SPI (MOSI=13, CLK=14, CS=15, DC=21, RST=22, BL=23) — verified working
- **Touch**: FT5x06 capacitive I2C (SDA=18, SCL=19, addr=0x38) — verified detected on I2C scan
- **Internal SRAM**: ~365KB free, ~302KB DMA-capable
- **Serial port**: COM7 (CH340 USB-UART, driver must be installed manually)

## Emulation Architecture

### Boot Sequence

The emulator applies several bypasses to get through the Mac Classic ROM boot:

1. **Memory test bypass** — ROM tests at $401CF0, $401D80, $401D96 force D6=0
2. **SCSI polling bypass** — Clear loop counters at $41A44A (unconditional — must remain active during INIT loading)
2b. **SCSI boot_mode** — `mac_scsi.c` returns $00 for all reads during boot (prevents SCSI Manager heap allocation that crashes INIT loading). Enabled via `scsi_set_active(true)` at idle loop detection.
3. **Overlay one-shot latch** — First ORA write clears ROM overlay permanently
4. **I/O completion bypass** — Fulfills disk reads directly from embedded flash image
5. **StartUpScreen patch** — BNE→BRA to skip missing DSAT resource
6. **ADB polling bypass** — Writes $FF completion flag at $08446C every frame
7. **VIA PB4+PB5 force high** — ADB bus idle bits in ORB reads
8. **VIA SR interrupt emulation** — ADB shift register protocol (gated by `adb_active` flag)
9. **SCC RR0 Sync/Hunt bit** — Bit 4 set for LocalTalk INIT compatibility
10. **ROM patches** — Mini vMac approach: checksum skip ($1C68), RAM test skip ($0086, $011E)
11. **UNIMP trap handler** — Returns SUBA.L A0,A0; MOVEQ #0,D0; RTS at $400768
12. **Device Manager intercepts** — read16_dm_read/ctl/stat for .SONY disk I/O
13. **SEC-RD drive bypass** — SR flag manipulation at $4357F4 for drive ≥ 4
14. **DM-spin rescue** — Instruction-fetch intercept at $4031BE catches stuck .SONY _Read polling (ioResult=1). `read16_dm_spin_rescue()` in mac_memory.c force-completes I/O from embedded disk image. Without this, async _Read requests that miss the $40315C intercept spin forever at 53% real-time.

### Performance Optimizations

1. **M68K_SEPARATE_READS** (`m68kconf.h`) — Instruction fetches use lean IRAM functions that skip all diagnostic intercepts. Data reads keep full intercept logic. This gave a **17% reduction** in execution cycles.
2. **mac_post_boot flag** (`mac_memory.h`) — Set at F2000. Enables the lean fetch path. During boot, falls through to the full `m68k_read_memory_16` with all intercepts.
3. **IRAM_ATTR** — 5 lean read functions placed in internal SRAM (728 bytes, 70KB headroom). Bypasses flash instruction cache for guaranteed fast access.
4. **PC consolidation** — Single `m68k_get_reg(PC)` per slice shared by all post-execute checks (was 8 separate calls).
5. **Cycle profiling** — `esp_cpu_get_cycle_count()` measures exec vs overhead. Logged with SPEED report every 300 frames. Shows 97% time in m68k_execute, 3% overhead.
6. **Idle fast-forward** (`MAC_IDLE_FF`, default 1, `mac_emulator.h/.c`) — when the Mac only waits for an interrupt (Process Manager idle loop `_Trap $ABF7; MOVE.L A4,D0; BEQ` seen from the A-line hook in `m68ki_exception_1010` with no interrupt/frame end since the previous pass, or a `CMP.L ($016A).W,Dn; Bcc.S *-4` tick-wait at a slice end), the rest of the frame's cycles are not emulated: device time (VIA/Egret/VBL) advances in `fast_slice` chunks until an interrupt above the SR mask is pending. Ticks per frame are unchanged. Host: LC II Finder idle 20.8k → 3.4k instructions/frame, Classic 10.2k → 3.7k, window drag 21.7k → 1.3k. Logged as `IDLE-FF:` with SPEED.
7. **Real-time throttle** (`MAC_FRAME_THROTTLE`, default 1 on ESP32, 0 on host) — at most one frame per 16.6 ms (vTaskDelay); needed once idle frames are cheap. SPEED therefore caps at ~100%.
8. **V8 cycle scale** (`MAC_V8_CYCLE_SCALE`, default 2) — the LC II CPU gets `MAC_CYCLES_PER_VBLANK/2` Musashi cycles per frame; each CPU cycle advances device time (VIA E clock, Egret, VBL) by 2. Musashi's 68030 timings (~6.2 cyc/instr vs 12.8 on the 68000) otherwise gave the LC II twice the instructions per tick. SWIM byte timing stays in CPU cycles. Boot-to-Finder takes ~2x the frames (same instructions); TimeDBRA calibrates to half (630 vs 1192).

### CRITICAL: VIA Timer Slice Size

`fast_slice` in `mac_emulator.c` **must stay ≤ 4000 cycles**. Tested exhaustively up to 40000 with and without timer arm tracking — crashes deterministically at F5251+ (PC enters SCSI range $2Exxxx). The VIA T2 timer often has periods of 100-2000 cycles; larger slices cause `via_tick()` to miss underflows. This is **unsolvable** without running via_tick inside Musashi's execution callback.

### Memory Map (Mac Classic, normal mode)

| Address Range | Device |
|---------------|--------|
| $000000-$1FFFFF | RAM (2MB, aliased via mask) |
| $400000-$43FFFF | ROM (512KB) |
| $580000-$5FFFFF | SCSI (NCR 5380 — full register emulation, no devices) |
| $800000-$9FFFFF | SCC read |
| $A00000-$BFFFFF | SCC write |
| $C00000-$DFFFFF | IWM/SWIM |
| $E00000-$EFFFFF | VIA + SWIM ISM (A12 bank split) |

### Touch Input (Trackpad-Style Mouse)

The FT5x06 capacitive touch panel acts as a relative trackpad with tap-to-click:

- **Swipe/drag** (>8px movement): moves cursor, NO button press
- **Quick tap** (<300ms, <8px movement): registers a click (mouseDown + mouseUp)
- **Tap-then-drag** (second touch within 400ms of tap): click-and-drag for menus/windows
- **Lift finger**: stops cursor movement / ends drag

Implementation (`mac_touch.c` gestures/sampler, `mac_emulator.c` apply):
0. `TOUCH_SAMPLER_TASK` (default 1 on ESP32, 0 on host): a FreeRTOS task on core 0 (`touch_sampler_start()` from main.cpp) samples at ~60 Hz independent of emulation speed; the emulation loop calls `touch_consume()` once per frame (spinlock-guarded queue: movement + ordered button transitions, each button state held >= 2 frames). With 0, `touch_poll_inline()` samples from the loop every 16 ms.
   Once a touch crosses 8 px the movement since touch-down is applied (no dead zone); after 300 ms a 3 px nudge also counts as a drag.
   **LC II / V8**: cursor positioned absolutely like the Classic (`v8_apply_touch()`: MTemp/RawMouse/Mouse = Mouse + delta, CrsrNew if CrsrCouple) — 1:1, no acceleration. The emulated ADB mouse carries the button only (zero deltas); movement after a button transition waits until that transition is in an ADB packet.
1. Polls FT5x06 at ~60Hz via `display_get_touch()` (I2C)
2. Touch axes are swapped and horizontal inverted vs display orientation
3. Gesture detection: tracks touch start time/position, classifies as tap or drag
4. Cursor delta applied only during drags (not on first touch frame)
5. Mouse button via **three layers**:
   - **Event queue posting** (`mac_post_mouse_event()`) — Posts mouseDown/mouseUp events directly to the Mac OS event queue (EvQHdr at $014A). Uses fixed EvQEl structures at $00C0 and $00D8 (68010-reserved vector area). **This is the essential layer** — GetNextEvent/WaitNextEvent reads from the queue, not MBState.
   - **MBState ($0172)** bit 7 — software mirror (0=down, 1=up), written every frame for code that reads MBState directly (_Button trap)
   - **VIA PB3** (`via_set_mouse_button()`) — hardware button, active low. VIA IRB initialized to $31 (PB3=0, button down during boot — required for boot to work).
6. Per-frame writes cursor position to Mac low-memory globals (gated by `mac_post_boot` + WMgrPort valid, **only when position changes**):
   - MTemp ($0828), RawMouse ($082C), Mouse ($0830) — big-endian Mac Point (Y.hi Y.lo X.hi X.lo)
   - CrsrNew ($08CE) = $FF — triggers cursor redraw (**only written when position changes**)
   - CrsrCouple ($08CF) = $FF — keeps cursor coupled to mouse

**CRITICAL**: CrsrNew must NOT be written unconditionally every frame — it forces cursor redraws that crash the resource decompressor (DCMP) during INIT/Finder loading. Only set CrsrNew when mouse position actually changes. Initial tracking values must match `mouse_x`/`mouse_y` defaults to prevent a false change on first WMgrPort-valid frame.

**CRITICAL**: On ADB Macs (like Mac Classic), the event system does NOT poll MBState for click detection. Mouse events must be POSTED to the event queue at $014A. Setting MBState alone is insufficient — applications using WaitNextEvent/GetNextEvent/ModalDialog never see clicks without queue-posted events.

## Key Design Decisions

1. **Native esp_lcd driver** instead of `esp32_display_panel` managed component (incompatible with ESP-IDF v6.0)
2. **No disassembler** compiled in (`m68kdasm.c` excluded) to save 256KB DRAM
3. **PSRAM for Musashi tables** via `EXT_RAM_BSS_ATTR` — the 580KB opcode/cycle tables don't fit in 180KB DRAM
4. **Chunked video rendering** — 20-line strips to limit DMA buffer size
5. **Warning suppression** for Musashi (GCC 15.2.0 is stricter) — see `components/musashi/CMakeLists.txt`
6. **Disk image in flash** — embedded in flash (.rodata), read-only. Saves PSRAM for RAM.
7. **2MB Mac RAM** (power-of-2) — required for correct ROM sizing via hardware-style aliasing. Addresses beyond 2MB wrap via `ram_mask = 0x1FFFFF`. 2MB is a valid Mac Classic config (1MB base + 1MB SIMM).
8. **M68K_SEPARATE_READS** — Splits instruction fetches from data reads in Musashi for lean hot path. Lean functions go in IRAM; data reads keep all diagnostic intercepts.

## PSRAM Budget (~4MB usable)

| Allocation | Size | Source |
|------------|------|--------|
| Musashi opcode tables | 580KB | EXT_RAM_BSS (static) |
| Mac RAM | 2MB | heap_caps_malloc at boot |
| **Total** | **~2.6MB** | |
| Headroom | ~1.4MB | |

## Future Work

- **Further performance**: 97% of time is in m68k_execute(). Gains require optimizing the Musashi core (computed-goto dispatch, hot handlers in IRAM, a smaller jump table) and reducing PSRAM/flash cache pressure. A 68030 ROM is **not** a speed lever: speed is host instructions per second, and no ROM beats Classic.ROM (68000, 1-bit). fast_slice increase is confirmed impossible. PSRAM workaround `DUPLDST` was measured 1.3% slower than `MEMW` (keep MEMW; the chip is rev v1.0, so the workaround cannot be disabled).
- **68030 / colour (V8) track**: CPU `M68K_CPU_TYPE_68030` (Color Classic and LC II are both 68030). V8 device map from the ROM's decoder table: VIA1 $F00000, SWIM $F16000, SCC $F04000, SCSI $F10000, ASC $F14000, VDAC $F24000, pseudo-VIA2 $F26000, VRAM $F40000. The overlay clears on the first access to ROM at $A00000, not on a VIA ORA write. Remaining: Egret HLE, then video (VRAM, monitor sense, CLUT, depth-aware renderer). **Final ROM: LC-II.ROM** (saves 512KB of flash vs Color Classic).
- SCC serial emulation (currently stub)
- ~~Touch-screen mouse~~ — **DONE**: trackpad-style relative input with tap-to-click and tap-then-drag
- ~~Mouse clicks~~ — **DONE**: event queue posting to EvQHdr $014A
- Keyboard input via touch-screen virtual keyboard
- **Virtual SCSI hard drive** in PSRAM or flash partition for OS installation. NCR 5380 register emulation already works (arbitration, selection timeout, bus status). Need to add a SCSI target device that responds to selection and handles INQUIRY/READ/WRITE commands. Flash budget: ~1.44MB free (app partition 3.94MB, current binary 2.48MB).
- Strip diagnostic logging for production builds (would reduce flash code size but minimal speed impact since 97% is in Musashi)
