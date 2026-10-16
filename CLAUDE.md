# Macintosh Classic Emulator for ESP32

## Project Overview

A Macintosh Classic emulator running on the WT32-SC01-3.4 board (ESP32-WROVER-B).
Emulates a 68000 CPU via Musashi, with 512x342 monochrome display scaled to 480x320 RGB565 on an ST7796 SPI LCD with FT5x06 capacitive touch.

**Current status**: System 7.5.3 Finder launches and runs stably with trackpad-style touch input and working mouse clicks. Full boot sequence completes (ROM self-test → boot block → System file → INITs → _InitWindows → _Launch → Finder). Speed: 39-91% real-time (peak ~91% at idle, 57-67% during boot). Touch screen works as relative trackpad (swipe to move cursor without clicking, tap to click, tap-then-drag for click-and-drag).

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
│   ├── build.bat             # ESP-IDF v6.0 build wrapper (must use cmd.exe)
│   ├── partitions.csv        # Custom partition table (4MB flash)
│   └── sdkconfig.defaults    # SDK configuration
├── ROM/                      # ROM and disk images (not in git, gitignored)
│   ├── Classic.ROM           # 512KB Mac Classic ROM (required)
│   ├── OS_750_boot.dsk       # 1.44MB System 7.5.3 boot floppy
│   ├── Color-Classic.ROM     # For future 68030 upgrade
│   └── LC-II.ROM             # For future 68030 upgrade
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

### Musashi Code Generation

Musashi requires a two-stage build. The generated files (`m68kops.c`, `m68kops.h`) are already committed and should not need regeneration unless `m68k_in.c` changes.

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
- **Flash**: **4MB** (detected, despite "16MB" label on board), 80MHz DIO
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

Implementation (`mac_emulator.c`):
1. Polls FT5x06 at ~60Hz via `display_get_touch()` (I2C, core 1)
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

- **Further performance**: 97% of time is in m68k_execute(). Gains require optimizing Musashi core (e.g., computed goto dispatch, cycle table in IRAM), reducing PSRAM pressure, or upgrading to 68030 (fewer cycles per instruction). fast_slice increase is confirmed impossible.
- **68030 upgrade**: Musashi supports 68030 emulation. Change `M68K_EMULATE_030` to `OPT_ON` in `m68kconf.h`, set CPU type to `M68K_CPU_TYPE_68030`, use a 68030-era ROM (Color Classic or LC II).
- SCC serial emulation (currently stub)
- ~~Touch-screen mouse~~ — **DONE**: trackpad-style relative input with tap-to-click and tap-then-drag
- ~~Mouse clicks~~ — **DONE**: event queue posting to EvQHdr $014A
- Keyboard input via touch-screen virtual keyboard
- **Virtual SCSI hard drive** in PSRAM or flash partition for OS installation. NCR 5380 register emulation already works (arbitration, selection timeout, bus status). Need to add a SCSI target device that responds to selection and handles INQUIRY/READ/WRITE commands. Flash budget: ~1.44MB free (app partition 3.94MB, current binary 2.48MB).
- Strip diagnostic logging for production builds (would reduce flash code size but minimal speed impact since 97% is in Musashi)
