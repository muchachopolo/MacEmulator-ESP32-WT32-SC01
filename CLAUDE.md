# Macintosh Classic Emulator for ESP32

## Project Overview

A Macintosh Classic emulator running on the WT32-SC01-3.4 board (ESP32-WROVER-B).
Emulates a 68000 CPU via Musashi, with 512x342 monochrome display scaled to 480x320 RGB565 on an ST7796 SPI LCD with FT5x06 capacitive touch.

**Current status**: System 7.5.3 Finder launches and runs stably. Full boot sequence completes (ROM self-test → boot block → System file → INITs → _InitWindows → _Launch → Finder). Speed: 48-70% real-time.

## Repository Layout

```
D:\Personal\Macintosh\
├── MacintoshESPEmulator/     # ESP-IDF project (the emulator)
│   ├── main/                 # Application source code
│   │   ├── mac_emulator.c    # Main emulation loop + boot bypasses
│   │   ├── mac_memory.c      # Memory map + trap intercepts + disk I/O
│   │   ├── mac_via.c         # VIA 6522 (timers, interrupts, ADB, overlay)
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
2. **SCSI polling bypass** — No SCSI device, clear loop counters at $41A44A
3. **Overlay one-shot latch** — First ORA write clears ROM overlay permanently
4. **I/O completion bypass** — Fulfills disk reads directly from embedded flash image
5. **StartUpScreen patch** — BNE→BRA to skip missing DSAT resource
6. **ADB polling bypass** — Writes $FF completion flag at $08446C every frame
7. **VIA PB4+PB5 force high** — ADB bus idle bits in ORB reads
8. **VIA SR interrupt emulation** — ADB shift register protocol (gated by `adb_active` flag)
9. **SCC RR0 Sync/Hunt bit** — Bit 4 set for LocalTalk INIT compatibility

### CRITICAL: VIA Timer Slice Size

`fast_slice` in `mac_emulator.c` **must stay ≤ 4000 cycles**. The VIA T2 timer often has periods of 100-2000 cycles. With larger slices, `via_tick()` misses timer underflows, corrupting interrupt timing. This was the root cause of a crash at F8000 (PC=$C80584, SP=$000000) seen with fast_slice=40000.

### Memory Map (Mac Classic, normal mode)

| Address Range | Device |
|---------------|--------|
| $000000-$1FFFFF | RAM (2MB, aliased via mask) |
| $400000-$43FFFF | ROM (512KB) |
| $580000-$5FFFFF | SCSI (NCR 5380 stub) |
| $800000-$9FFFFF | SCC read |
| $A00000-$BFFFFF | SCC write |
| $C00000-$DFFFFF | IWM/SWIM |
| $E00000-$EFFFFF | VIA + SWIM ISM (A12 bank split) |

## Key Design Decisions

1. **Native esp_lcd driver** instead of `esp32_display_panel` managed component (incompatible with ESP-IDF v6.0)
2. **No disassembler** compiled in (`m68kdasm.c` excluded) to save 256KB DRAM
3. **PSRAM for Musashi tables** via `EXT_RAM_BSS_ATTR` — the 580KB opcode/cycle tables don't fit in 180KB DRAM
4. **Chunked video rendering** — 20-line strips to limit DMA buffer size
5. **Warning suppression** for Musashi (GCC 15.2.0 is stricter) — see `components/musashi/CMakeLists.txt`
6. **Disk image in flash** — embedded in flash (.rodata), read-only. Saves PSRAM for RAM.
7. **2MB Mac RAM** (power-of-2) — required for correct ROM sizing via hardware-style aliasing. Addresses beyond 2MB wrap via `ram_mask = 0x1FFFFF`. 2MB is a valid Mac Classic config (1MB base + 1MB SIMM).

## PSRAM Budget (~4MB usable)

| Allocation | Size | Source |
|------------|------|--------|
| Musashi opcode tables | 580KB | EXT_RAM_BSS (static) |
| Mac RAM | 2MB | heap_caps_malloc at boot |
| **Total** | **~2.6MB** | |
| Headroom | ~1.4MB | |

## Future Work

- **Performance optimization**: Make `via_tick()` handle large cycle counts mathematically so `fast_slice` can be increased beyond 4000 without timer accuracy loss
- **68030 upgrade**: Musashi supports 68030 emulation. Change `M68K_EMULATE_030` to `OPT_ON` in `m68kconf.h`, set CPU type to `M68K_CPU_TYPE_68030`, use a 68030-era ROM (Color Classic or LC II).
- SCC serial emulation (currently stub)
- Keyboard input via touch-screen virtual keyboard
- **Virtual SCSI hard drive** in PSRAM or flash partition for OS installation
- Strip diagnostic logging for production builds
