# Macintosh Classic Emulator for ESP32 — Architecture Document

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Prerequisites — What You Need to Know](#2-prerequisites--what-you-need-to-know)
3. [Hardware Platform](#3-hardware-platform)
4. [Emulated Hardware — The Macintosh Classic](#4-emulated-hardware--the-macintosh-classic)
5. [Software Architecture](#5-software-architecture)
6. [The CPU — Musashi 68000 Emulator](#6-the-cpu--musashi-68000-emulator)
7. [Memory Map and Address Decoding](#7-memory-map-and-address-decoding)
8. [The Boot Sequence and ROM Bypasses](#8-the-boot-sequence-and-rom-bypasses)
9. [VIA 6522 Emulation](#9-via-6522-emulation)
10. [IWM/SWIM Floppy Disk Emulation](#10-iwmswim-floppy-disk-emulation)
11. [SCC Z8530 Serial Emulation](#11-scc-z8530-serial-emulation)
12. [Video Pipeline](#12-video-pipeline)
13. [Touch Input — Trackpad-Style Mouse](#13-touch-input--trackpad-style-mouse)
14. [Performance Optimization](#14-performance-optimization)
15. [Emulation Loop — Frame by Frame](#15-emulation-loop--frame-by-frame)
16. [PSRAM Budget](#16-psram-budget)
17. [Build System](#17-build-system)
18. [Challenges Solved — Debugging History](#18-challenges-solved--debugging-history)
19. [Current Status and Future Work](#19-current-status-and-future-work)
20. [Glossary](#20-glossary)

---

## 1. Project Overview

This project is a **Macintosh Classic emulator running on an ESP32 microcontroller**.
It boots the original Macintosh Classic ROM, loads System 7.5.3 from a disk image
embedded in flash, and runs the Finder desktop with touch-screen mouse input.

**What it emulates:**
- Motorola 68000 CPU at 7.8336 MHz (via the Musashi emulation library)
- 2 MB RAM, 512 KB ROM
- VIA 6522 (interrupts, timers, keyboard/ADB, overlay control)
- IWM/SWIM floppy disk controller (MFM sector reads from embedded disk image)
- SCC Z8530 serial controller (stub with safe defaults)
- SCSI NCR 5380 (stub — bus free, no devices)
- 512x342 monochrome video, scaled to 480x320 RGB565 on an SPI LCD
- Capacitive touch input as trackpad-style mouse

**Current state (April 2026):** System 7.5.3 Finder launches and runs stably with
working mouse clicks. Speed is 39-91% real-time (peak ~91% at idle, 57-67% during
boot). Touch screen works as a relative trackpad with gesture detection (swipe to
move cursor without clicking, tap to click, tap-then-drag for click-and-drag).

### File Layout

```
MacintoshESPEmulator/
├── main/
│   ├── main.cpp          # ESP-IDF entry point, display init, task creation
│   ├── mac_emulator.c    # Main emulation loop, boot bypasses, touch input
│   ├── mac_emulator.h    # Mac Classic constants (clock, screen, timing)
│   ├── mac_memory.c      # 24-bit address bus decode, Musashi memory callbacks
│   ├── mac_memory.h      # Memory subsystem API
│   ├── mac_via.c         # VIA 6522 emulation (ports, timers, interrupts)
│   ├── mac_via.h         # VIA register definitions, IRQ bit masks
│   ├── mac_scc.c         # SCC Z8530 stub (register pointer, safe defaults)
│   ├── mac_iwm.c         # IWM/SWIM floppy (ISM mode, MFM sector reads)
│   └── mac_video.c       # 1-bit to RGB565 converter, chunked DMA renderer
├── components/
│   ├── musashi/          # Musashi 68000 CPU emulator (third-party, copied)
│   │   ├── m68kcpu.c     # CPU core (instruction dispatch, execute loop)
│   │   ├── m68kops.c     # Generated opcode handlers (from m68kmake)
│   │   ├── m68kconf.h    # Configuration: CPU type, callbacks, optimizations
│   │   └── ...
│   └── display/          # Native esp_lcd display driver
│       └── display.cpp   # ST7796 SPI + FT5x06 I2C touch
├── build.bat             # ESP-IDF v6.0 build wrapper (must use cmd.exe)
├── partitions.csv        # Custom partition table (4MB flash)
└── sdkconfig.defaults    # SDK configuration
```

---

## 2. Prerequisites — What You Need to Know

To fully understand this emulator, you need knowledge in several areas. Here is
what each area covers and where to learn more.

### 2.1 Motorola 68000 Architecture

The 68000 is a 16/32-bit CISC processor from 1979. Key concepts:

- **Registers**: 8 data registers (D0-D7), 8 address registers (A0-A7), A7 is
  the stack pointer (SP). Program Counter (PC), Status Register (SR).
- **24-bit address bus**: Only 16 MB of address space (addresses are 32-bit but
  the top 8 bits are ignored on the original 68000).
- **Big-endian byte order**: The most significant byte is at the lowest address.
  A 32-bit value `0x12345678` stored at address N has `0x12` at N, `0x34` at N+1,
  `0x56` at N+2, `0x78` at N+3.
- **Exception vectors**: The first 256 longwords (1 KB) at address 0 are the
  exception vector table. Vector 0 = initial SSP, Vector 1 = initial PC,
  Vector 2 = bus error, etc.
- **Supervisor vs User mode**: SR bit 13 selects mode. The Mac ROM runs in
  supervisor mode.
- **A-line traps**: Opcodes starting with `$Axxx` trigger a Line-A exception.
  The Mac OS uses these as system call entry points (trap dispatch).

**Resources:**
- "M68000 Programmer's Reference Manual" (Motorola) — the definitive CPU reference
- "68000 Assembly Language Programming" (Leventhal) — gentler introduction

### 2.2 Macintosh Classic Hardware

The Mac Classic (1990) is a compact all-in-one Macintosh with:

- 68000 CPU at 7.8336 MHz
- 1-4 MB RAM
- 512 KB ROM containing the Toolbox, OS, and boot code
- 512x342 monochrome built-in display
- 3.5" 1.44 MB SuperDrive (MFM format)
- No hard drive (external SCSI only)

Key chips and their roles:
- **VIA 6522**: System timer, interrupts, keyboard/ADB, overlay control
- **SCC Z8530**: Two serial ports (modem + printer/LocalTalk)
- **SWIM**: SuperDrive floppy controller (IWM successor, MFM capable)
- **NCR 5380**: SCSI bus controller
- **Gemini/GLU**: Address decoder + chip select (replaced PAL on earlier Macs)

**Resources:**
- "Guide to the Macintosh Family Hardware" (Apple, 2nd ed.) — the hardware bible
- "Inside Macintosh: Hardware" (Apple) — detailed peripheral descriptions
- "Designing Cards and Drivers for the Macintosh Family" — address map details

### 2.3 Macintosh System Software Internals

The Mac OS runs on top of the ROM. Key concepts:

- **Low-memory globals**: Fixed addresses in the first ~3 KB of RAM that the OS
  uses as global variables. Examples: `MemTop` ($0108), `ScrnBase` ($0824),
  `Mouse` ($0830), `MBState` ($0172). The emulator reads and writes these
  directly to interact with the running OS.
- **Trap dispatch**: System calls use A-line opcodes. Bits 11-0 select the trap
  number. The OS trap table is at $0400-$07FF, Toolbox at $0E00-$15FF.
- **Memory Manager**: Zone-based heap allocator. `SysZone` starts at $1600,
  `ApplZone` starts higher. Handles, pointers, master pointer blocks.
- **Resource Manager**: Files are structured as resource forks containing typed,
  numbered chunks (ICONs, CODE, INIT, etc.). The System file is a resource fork.
- **Boot sequence**: ROM self-test → copy vectors → clear overlay → memory test →
  SCC init → disk init → read boot blocks → load System file → run INITs →
  launch Finder.
- **INITs**: Code resources loaded at boot time (like startup extensions). They
  run sequentially, each can install drivers, patches, etc.

**Resources:**
- "Inside Macintosh" volumes I-VI — the official OS reference
- "Macintosh Revealed" (Stephen Chernicoff) — explains low-memory globals
- Mini vMac source code — reference emulator, invaluable for ROM patches

### 2.4 ESP32 and ESP-IDF

The host platform is an ESP32 microcontroller running ESP-IDF (Espressif's
FreeRTOS-based SDK). Key concepts:

- **Dual-core Xtensa LX6** at 240 MHz. Core 0 runs video rendering, Core 1
  runs the emulation loop.
- **PSRAM**: External SPI RAM (8 MB physical, ~4 MB usable due to ESP32 virtual
  address space limitation). Used for Mac RAM and Musashi's large tables.
- **Internal SRAM**: ~365 KB, fast access. Used for FreeRTOS, DMA buffers,
  and critical code (IRAM).
- **Flash**: 4 MB. Holds firmware code + embedded ROM and disk images.
- **IRAM_ATTR**: Attribute that places a function in internal SRAM for
  guaranteed single-cycle access (bypasses flash instruction cache).
- **DMA**: The SPI peripheral has a DMA engine for display transfers. DMA
  buffers must be in internal SRAM (not PSRAM).
- **FreeRTOS tasks**: The OS scheduler runs tasks on both cores. Tasks are
  pinned to specific cores for deterministic behavior.
- **heap_caps_malloc()**: Memory allocator that can target specific memory
  types (PSRAM, internal, DMA-capable).
- **EXT_RAM_BSS_ATTR**: Attribute that places a static variable in PSRAM
  instead of internal SRAM.

**Resources:**
- ESP-IDF Programming Guide (docs.espressif.com)
- ESP32 Technical Reference Manual — SPI, DMA, memory map
- FreeRTOS documentation (freertos.org)

### 2.5 SPI LCD and Touch

The display uses:
- **ST7796**: SPI LCD controller (480x320, RGB565). Commands and pixel data are
  sent over SPI at 40 MHz. The ESP-IDF `esp_lcd` API abstracts the protocol.
- **FT5x06**: Capacitive touch controller on I2C (400 kHz). Returns touch
  coordinates as raw register reads.

### 2.6 Floppy Disk Format (MFM)

The Mac's 1.44 MB SuperDrive uses IBM-compatible MFM (Modified Frequency
Modulation) encoding:
- 80 tracks, 2 sides, 18 sectors per track
- Each sector: 512 bytes of data
- Sector format on disk: gap → sync (12x $00 + 3x $A1) → ID mark ($FE) →
  track/side/sector/size → CRC → gap → sync → data mark ($FB) → 512 data
  bytes → CRC
- CRC: CRC-CCITT (polynomial $1021, init $FFFF)

The SWIM chip reads this bitstream and presents bytes to the CPU through its
ISM (Integrated SWIM Module) registers. The emulator synthesizes this byte
stream from the raw disk image.

---

## 3. Hardware Platform

### Board: WT32-SC01-3.4 (Wireless Tag)

```
+----------------------------------------------+
|  WT32-SC01-3.4                               |
|                                              |
|  +--------+    +----------+    +---------+   |
|  | ESP32   |    | ST7796   |    | FT5x06 |   |
|  | D0WD    |--->| 480x320  |    | Touch   |   |
|  | 240MHz  | SPI| SPI LCD  |    | I2C     |   |
|  | 4MB Fl  |    +----------+    +---------+   |
|  | 8MB PS  |                                  |
|  +--------+                                   |
|             CH340 USB-UART (COM7)             |
+----------------------------------------------+
```

| Component | Specification |
|-----------|---------------|
| SoC | ESP32-D0WD rev v1.0, dual-core Xtensa LX6 @ 240 MHz |
| Flash | 4 MB (80 MHz DIO) — board says "16MB" but only 4 MB detected |
| PSRAM | 8 MB physical, ~4 MB usable (ESP32 virtual address space limit) |
| Internal SRAM | ~365 KB free, ~302 KB DMA-capable |
| Display | ST7796 480x320 SPI (MOSI=13, CLK=14, CS=15, DC=21, RST=22, BL=23) |
| Touch | FT5x06 capacitive I2C (SDA=18, SCL=19, addr=0x38) |
| Serial | COM7 (CH340 USB-UART) |

---

## 4. Emulated Hardware — The Macintosh Classic

The Mac Classic has a simple architecture: a 68000 CPU with a 24-bit address
bus connects to RAM, ROM, and several I/O chips via address decoding logic.

```
                    24-bit Address Bus
                           |
  +-------+-------+--------+--------+-------+-------+
  |       |       |        |        |       |       |
  v       v       v        v        v       v       v
 RAM     ROM    SCSI     SCC      IWM     VIA    Unused
 2MB    512KB   5380    Z8530    SWIM    6522
$000000 $400000 $580000 $800000 $C00000 $E00000

  RAM: Macintosh main memory (heap, stack, screen buffer, system globals)
  ROM: Contains the Mac Toolbox, OS traps, boot code, drivers
  SCSI: Hard disk bus (stub — no devices)
  SCC: Two serial ports (stub — safe defaults)
  IWM/SWIM: Floppy disk controller (full ISM sector read emulation)
  VIA: System timer, interrupts, ADB/keyboard, ROM overlay
```

### What the Emulator Must Provide

For the Mac ROM to boot to Finder, the emulator must:

1. **Execute 68000 instructions** — Musashi handles this
2. **Decode addresses** — route reads/writes to the correct device
3. **Emulate VIA timers and interrupts** — the OS depends on VBlank (~60 Hz)
   and Timer 1/Timer 2 for scheduling
4. **Handle the ROM overlay** — ROM must be at address 0 during reset, then
   swap to RAM
5. **Provide disk I/O** — the ROM reads boot blocks and System file from floppy
6. **Satisfy hardware probes** — SCC, SCSI, ADB must return sane values or
   the ROM hangs in polling loops
7. **Drive the display** — convert the Mac's 1-bit framebuffer to the LCD's
   RGB565 format
8. **Accept input** — touch screen maps to Mac mouse position and button state

---

## 5. Software Architecture

### Dual-Core Task Model

```
Core 0                              Core 1
┌─────────────────────┐             ┌────────────────────────┐
│  mac_video_task()   │             │  mac_emu_run()         │
│  - Read framebuffer │             │  - m68k_execute()      │
│  - Scale 512x342    │             │  - via_tick()          │
│  - Convert to RGB565│             │  - Boot bypasses       │
│  - DMA to LCD       │             │  - Touch input poll    │
│  - ~10 fps          │             │  - Mouse → Mac globals │
│                     │             │  - Frame timing        │
│  Priority: MAX-3    │             │  Priority: MAX-2       │
│  Stack: 8 KB        │             │  Stack: 32 KB          │
└─────────────────────┘             └────────────────────────┘
```

- **Core 1** runs the emulation loop (`mac_emu_run`). This is the hot path —
  97% of its time is spent inside `m68k_execute()` running 68000 instructions.
- **Core 0** runs the video renderer (`mac_video_task`). It continuously reads
  the Mac framebuffer from PSRAM and pushes it to the LCD via SPI DMA.
- The two tasks share Mac RAM (in PSRAM) without locking — the video task only
  reads, and frame tearing is acceptable at 10 fps.

### Initialization Flow (main.cpp)

```
app_main()
    │
    ├── display_init()           # SPI bus, LCD panel, I2C touch
    ├── mac_emu_init()
    │     ├── mac_mem_init()     # Allocate RAM in PSRAM, init VIA/SCC/IWM
    │     ├── mac_video_init()   # Build lookup tables, allocate DMA buffer
    │     ├── m68k_init()        # Initialize Musashi CPU
    │     └── m68k_pulse_reset() # Load SSP and PC from vectors
    │
    ├── xTaskCreatePinnedToCore(mac_emu_run, core=1)
    └── xTaskCreatePinnedToCore(mac_video_task, core=0)
```

### Musashi Memory Callback Architecture

Musashi is a "callback-based" CPU emulator. It does not own any memory — every
memory access calls back into our code:

```
Musashi executes instruction
    │
    ├── Instruction fetch → m68k_read_immediate_16(address)
    │                        (lean IRAM path post-boot, or full path during boot)
    │
    ├── Data read → m68k_read_memory_{8,16,32}(address)
    │                (full path: address decode → RAM/ROM/VIA/SCC/IWM)
    │
    └── Data write → m68k_write_memory_{8,16,32}(address, value)
                      (full path: address decode → RAM/VIA/SCC/IWM)
```

The `M68K_SEPARATE_READS` optimization (Section 14) splits instruction fetches
from data reads, allowing the instruction path to skip diagnostic intercepts.

---

## 6. The CPU — Musashi 68000 Emulator

### What is Musashi?

Musashi (by Karl Stenerud) is an open-source, cycle-accurate 68000/010/020/030/040
CPU emulator written in C. It is used by MAME and other emulators.

### How It Works

Musashi uses a **table-driven opcode dispatch**. On startup, `m68k_init()`
builds a 64K-entry opcode handler table mapping each possible 16-bit opcode
to its handler function. During execution, `m68k_execute(cycles)`:

1. Fetches the next opcode word via `m68k_read_immediate_16(PC)`
2. Looks up the handler in the opcode table
3. Calls the handler, which may fetch additional words (extension words,
   immediate data) and perform the operation
4. Deducts the instruction's cycle count from the remaining budget
5. Repeats until the cycle budget is exhausted

The opcode table and cycle count table together consume ~580 KB — too large for
ESP32's internal SRAM. They are placed in PSRAM via `EXT_RAM_BSS_ATTR`.

### Configuration (m68kconf.h)

Key settings for this project:

| Setting | Value | Purpose |
|---------|-------|---------|
| M68K_CPU_TYPE | 68000 | Only 68000 enabled, no 010/020/030/040 |
| M68K_SEPARATE_READS | OPT_ON | Separate instruction fetch from data read callbacks |
| M68K_EMULATE_PREFETCH | OPT_OFF | Skip prefetch emulation (saves a branch per fetch) |
| M68K_EMULATE_ADDRESS_ERROR | OPT_OFF | Skip alignment checks (saves setjmp per execute) |
| M68K_INT_ACK_CALLBACK | mac_int_ack_handler | Autovectored interrupt acknowledge |
| M68K_RESET_CALLBACK | mac_reset_handler | Handle RESET instruction |

### Interrupt Handling

The Mac uses **autovectored interrupts**. The VIA generates a level-1 interrupt
request. When the 68000 acknowledges it:

1. The emulation loop calls `m68k_set_irq(1)` when `via_irq_pending()` is true
2. Musashi checks if the interrupt priority exceeds the CPU's interrupt mask
3. If accepted, it calls `mac_int_ack_handler(1)` which returns
   `M68K_INT_ACK_AUTOVECTOR`
4. The 68000 pushes PC and SR, then fetches the vector from address
   `0x000064 + (level * 4)` = `0x000068` for level 1

---

## 7. Memory Map and Address Decoding

### The 24-Bit Address Bus

The 68000 has a 24-bit address bus (16 MB address space). The top 8 bits of any
32-bit address are masked off: `address &= 0x00FFFFFF`.

### Normal Mode (Overlay OFF)

```
$000000 ┌──────────────────┐
        │   RAM (2 MB)     │  Aliased via ram_mask = 0x1FFFFF
        │                  │  Addresses $000000-$3FFFFF all map to
        │                  │  the same 2 MB (address & 0x1FFFFF)
$400000 ├──────────────────┤
        │   ROM (512 KB)   │  Aliased via rom_mask = 0x7FFFF
$500000 ├──────────────────┤
        │   (unused)       │
$580000 ├──────────────────┤
        │  SCSI NCR 5380   │  Returns $00 (bus free, no devices)
$600000 ├──────────────────┤
        │   (unused)       │
$800000 ├──────────────────┤
        │  SCC Read        │  Z8530 control/data read
$A00000 ├──────────────────┤
        │  SCC Write       │  Z8530 control/data write
$C00000 ├──────────────────┤
        │  IWM/SWIM        │  Floppy disk controller
$E00000 ├──────────────────┤
        │  VIA 6522        │  System control, timers, interrupts
        │  (+SWIM ISM      │  A12=1 routes to SWIM for driver code
        │   bank split)    │
$F00000 ├──────────────────┤
        │   (unused)       │
$FFFFFF └──────────────────┘
```

### Overlay Mode (at Reset)

At power-on, the VIA overlay flip-flop is SET, mapping ROM to address 0 so the
68000 can read its reset vectors (SSP at $000000, PC at $000004):

- $000000-$3FFFFF: ROM (instead of RAM)
- $600000-$7FFFFF: RAM (accessible at alternate address)

After the ROM copies exception vectors from ROM to RAM and writes to VIA ORA,
the overlay clears permanently (one-shot latch). See Section 9.

### RAM Layout (2 MB)

```
$000000 ┌──────────────────┐
        │ Exception Vectors │ $000-$3FF (1 KB) — 68000 vector table
        ├──────────────────┤
        │ Low-Memory Globals│ $100-$0FFF — OS variables used by ROM/System
        ├──────────────────┤
        │ OS Trap Table     │ $0400-$07FF — 256 OS trap dispatch addresses
        ├──────────────────┤
        │ Toolbox Trap Table│ $0E00-$15FF — 512 Toolbox trap dispatch addresses
        ├──────────────────┤
        │ System Heap       │ $1600-$6DF3 (SysZone)
        ├──────────────────┤
        │ Application Heap  │ $6DF4+ (ApplZone) — Finder's memory
        ├──────────────────┤
        │     ...           │
        ├──────────────────┤
        │ Screen Buffer     │ $1FA700 (RAM_SIZE - $5900)
        │ (21,888 bytes)    │ 512x342 pixels, 1 bit/pixel
        ├──────────────────┤
        │ Sound Buffer      │ $1FFD00 (RAM_SIZE - $0300)
        │ (768 bytes)       │
$200000 └──────────────────┘
```

### Key Low-Memory Globals

These fixed RAM addresses are used by the Mac OS. The emulator reads/writes
them directly to interact with the running system:

| Address | Name | Size | Purpose |
|---------|------|------|---------|
| $0108 | MemTop | Long | Top of physical memory |
| $0172 | MBState | Byte | Mouse button state (bit 7: 0=down, 1=up) |
| $0210 | BootDrive | Word | Drive number for boot disk |
| $02AA | ApplZone | Long | Pointer to application heap zone |
| $02A6 | SysZone | Long | Pointer to system heap zone |
| $0824 | ScrnBase | Long | Pointer to screen framebuffer |
| $0828 | MTemp | Point | Raw mouse position (before acceleration) |
| $082C | RawMouse | Point | Raw mouse position |
| $0830 | Mouse | Point | Current mouse position |
| $08CE | CrsrNew | Byte | $FF = trigger cursor redraw |
| $08CF | CrsrCouple | Byte | $FF = cursor tracks mouse position |
| $0AE0 | FinderName | Str | Name of application to launch |
| $0AF0 | DSErrCode | Word | System error code |

A Mac "Point" is 4 bytes: Y.hi, Y.lo, X.hi, X.lo (big-endian, vertical first).

### Address Decode Implementation

In `mac_memory.c`, the functions `mem_read_byte()` and `mem_write_byte()` implement
the address decoder as cascading if-else checks:

```c
address &= 0x00FFFFFF;  // Mask to 24-bit

if (overlay_active) {
    // ROM at $000000, RAM at $600000
} else {
    if (address < 0x400000)     return ram[address & ram_mask];
    if (address < 0x500000)     return rom[address & rom_mask];
}
if (address >= 0x580000 && address < 0x600000) return 0x00;  // SCSI
if (address >= 0x800000 && address < 0xA00000) return scc_read(address);
if (address >= 0xC00000 && address < 0xE00000) return iwm_read(address);
if (address >= 0xE00000 && address < 0xF00000) {
    // VIA, with A12 bank split for SWIM ISM registers
    if ((address & 0x1000) && pc_in_swim_driver) return iwm_read(address);
    return via_read(address);
}
return 0xFF;  // Unmapped space
```

### The VIA/SWIM A12 Bank Split

The Mac Classic shares the $E00000 address range between the VIA and the SWIM
chip's ISM registers. On real hardware, the Gemini chip routes accesses based on
address bit A12:
- A12=0: VIA registers
- A12=1: SWIM ISM registers (when accessed from the SWIM driver code)

The emulator checks both A12 and the current PC to determine routing. Only when
the PC is in the SWIM driver ROM ($434000-$436000) AND A12=1 does the access
go to the IWM/SWIM module. This prevents interrupt handlers reading VIA registers
from accidentally hitting the SWIM.

---

## 8. The Boot Sequence and ROM Bypasses

The Mac Classic ROM performs extensive hardware initialization. Several steps
assume real hardware that the emulator cannot fully provide, requiring targeted
bypasses. Each bypass was discovered through serial monitor debugging.

### 8.1 Normal Boot Flow

```
Power On / Reset
    │
    ├── 68000 reads SSP from $000000 (ROM via overlay)
    ├── 68000 reads PC from $000004 (ROM via overlay)
    │
    v
ROM Entry Point
    │
    ├── Copy exception vectors from ROM to RAM
    ├── First ORA write to VIA → overlay OFF permanently
    ├── Memory tests (D6 = error flags) ──── [Bypass: force D6=0]
    ├── VIA PA0 check (diagnostic device?)
    │     PA0=1 → continue normal boot
    │     PA0=0 → enter serial debug monitor ── [Bypass: PA0=1 in IRA]
    │
    ├── SCC initialization
    │     Sends "*APPLE*" on channel A
    │     Enters dispatch loop (serial debug monitor)
    │     ── [Bypass: redirect PC to ROM's own register-restore+RTS]
    │
    ├── IWM/SWIM initialization
    │     Probes for ISM mode, initializes MFM format
    │     ── [Multiple ISM patches — see Section 10]
    │
    ├── Read boot blocks from floppy (sectors 0-1)
    │     ── [Device Manager intercepts fulfill reads from flash]
    │
    ├── Execute boot block code
    │     Load System file, run INITs
    │     ── [ADB bypass, StartUpScreen bypass, UNIMP handler]
    │
    ├── _InitWindows, _InitMenus, _InitDialogs
    │     ── [SCC Sync/Hunt bit for LocalTalk INIT]
    │
    ├── _Launch Finder
    │
    v
  Finder Running (mac_post_boot = true at frame 2000)
```

### 8.2 Key Bypasses Explained

#### Overlay One-Shot Latch
The first write to VIA ORA clears the overlay permanently. Real Mac hardware
uses a PAL flip-flop that cannot be re-set by software. Without this, later ORA
writes (for sound volume, screen buffer select) would re-enable overlay and
corrupt the address map.

#### Memory Test (D6 = 0)
At PCs $401CF0, $401D80, $401D96, the ROM checks D6 for memory test errors.
The emulator forces D6=0 ("all tests passed"). This is a safety net — with
correct overlay emulation, D6 is already 0.

#### SCC Dispatch Loop Escape
The ROM enters a serial debug monitor at $401F38 with **no exit path** (every
code path ends with BRA back into the loop). The bypass redirects PC to $401F2C,
the ROM's own register-restore sequence (MOVE.W (SP)+,SR; MOVEM.L (SP)+,...; RTS).

#### Device Manager Intercepts
The emulator intercepts specific ROM addresses in the .SONY disk driver to
fulfill disk read requests directly from the embedded flash image. When the ROM
calls _Read on the disk driver, the intercept copies the requested sectors from
flash into Mac RAM, sets the ioResult to 0 (success), and returns.

#### ADB Polling Bypass
The Mac uses the VIA shift register for Apple Desktop Bus (ADB) communication
with the keyboard and mouse. Since there is no real ADB controller, the emulator:
- Forces VIA ORB bits PB4+PB5 high (ADB idle state)
- Writes $FF to the ADB completion flag ($08446C) every frame
- Emulates VIA SR interrupts (shift register done) with a 200-cycle delay

#### StartUpScreen Patch
The ROM tries to load a DSAT (Death Screen Alert Table) resource that doesn't
exist on this disk. A BNE→BRA patch skips the missing resource check.

#### UNIMP Trap Handler
Many Toolbox traps are unimplemented (point to $400768). The emulator installs
a handler that returns cleanly: `SUBA.L A0,A0; MOVEQ #0,D0; RTS`.

---

## 9. VIA 6522 Emulation

The VIA (Versatile Interface Adapter) is the most critical peripheral. It handles:

- **VBlank interrupt** (CA1) — 60 Hz vertical blank, drives the Mac's event loop
- **One-second interrupt** (CA2) — 1 Hz clock for time-of-day
- **Timer 1** — system timer (free-running or one-shot)
- **Timer 2** — utility timer (always one-shot)
- **Shift Register** — ADB keyboard/mouse protocol
- **Port A** — overlay control, sound volume, screen buffer select
- **Port B** — RTC data, keyboard clock/data

### Register Map

VIA registers are at base $EFE1FE with 512-byte ($200) spacing. The register
index is extracted as `(address >> 9) & 0xF`.

| Index | Register | Mac Usage |
|-------|----------|-----------|
| 0 | ORB | RTC, keyboard, ADB bus state |
| 1 | ORA | Overlay (one-shot), sound, screen buffer |
| 2 | DDRB | Port B pin directions |
| 3 | DDRA | Port A pin directions |
| 4 | T1CL | Timer 1 counter low |
| 5 | T1CH | Timer 1 counter high (write loads + arms) |
| 6 | T1LL | Timer 1 latch low |
| 7 | T1LH | Timer 1 latch high |
| 8 | T2CL | Timer 2 counter low (read clears T2 IRQ) |
| 9 | T2CH | Timer 2 counter high (write loads + arms) |
| 10 | SR | Shift register (ADB data) |
| 11 | ACR | Timer modes, shift register mode |
| 12 | PCR | CA1/CA2/CB1/CB2 edge control |
| 13 | IFR | Interrupt flag register |
| 14 | IER | Interrupt enable register |
| 15 | ORA_NH | ORA without handshake (no CA1/CA2 clear) |

### Timer Operation

Timers count down from their loaded value. On underflow (counter wraps past 0):

**Timer 1 (ACR bit 6 selects mode):**
- One-shot: fires once after T1CH write, then disarms
- Free-running: fires on every underflow, counter reloads from latch

**Timer 2 (always one-shot):**
- Armed by writing T2CH
- Fires once on underflow, then disarms
- Reading T2CL clears the T2 interrupt flag

**CRITICAL**: The emulator calls `via_tick(cycles)` after each CPU execution
slice. The slice size (`fast_slice`) must stay ≤ 4000 cycles because VIA T2
often has periods of 100-2000 cycles. Larger slices miss underflows, corrupting
interrupt timing and causing deterministic crashes.

### Interrupt Dispatch

```
Event occurs (VBlank, timer underflow, SR complete)
    │
    v
Set corresponding bit in IFR
    │
    v
via_update_irq(): IFR bit 7 = (IFR & IER & 0x7F) != 0
    │
    v
Main loop: if via_irq_pending() → m68k_set_irq(1)
           else                  → m68k_set_irq(0)
    │
    v
68000 checks interrupt mask in SR
    │
    v
If accepted: push PC+SR, vector to $000068 (autovector level 1)
```

---

## 10. IWM/SWIM Floppy Disk Emulation

The IWM/SWIM is the most complex peripheral. It provides full MFM sector read
emulation, allowing the ROM to load System 7.5.3 from the embedded disk image.

### Two Operating Modes

**IWM mode**: Legacy mode from earlier Macs. Uses state-machine register
selection via address lines (CA0, CA1, CA2, LSTRB, Q6, Q7). Used during
initial drive detection and sense line polling.

**ISM mode**: The SWIM's native mode for MFM (1.44 MB) disks. Uses a
conventional register file with 8 registers (DATA, MARK, CRC, PARAM, PHASE,
SETUP, MODE0, HANDSHAKE). The ROM switches from IWM to ISM mode during boot.

### ISM Register Summary

| Index | Read | Write | Purpose |
|-------|------|-------|---------|
| 0 | DATA | DATA | MFM data bytes from disk |
| 1 | MARK | MARK | Sync byte detection ($A1 = sync) |
| 2 | CRC | CRC | Running CRC-CCITT value |
| 3 | PARAM | PARAM | Parameters for ISM commands |
| 4 | PHASE | PHASE | Step motor phase control |
| 5 | SETUP | SETUP | MFM/GCR select, clock speed |
| 6 | STATUS0 | MODE0 | RDDATA flag, motor state / mode config |
| 7 | HANDSHAKE | MODE1 | RDDATA ready, sense, motor / mode config |

### Sector Read Flow

When the ROM reads a sector, this sequence occurs:

```
ROM sets MODE0 to $80 (activate read mode)
    │
    v
ROM polls STATUS0 for RDDATA=1 (byte available)
    │
    v
ROM reads DATA → gets gap byte ($4E or $00)
    │ (repeat until sync mark)
    v
ROM polls MARK register → bit 0 = 1 means sync ($A1)
    │
    v
ROM reads DATA → gets ID address mark ($FE)
ROM reads DATA → gets track, side, sector, size
ROM reads CRC → checks header CRC
    │
    v
ROM searches for matching sector number
ROM reads 512 data bytes from DATA register
ROM reads CRC → checks data CRC
```

### MFM Byte Clock Model

The emulator models the physical disk rotation. The key insight is that bytes
arrive from the disk at a fixed rate (~125 CPU cycles per byte at 500 kbps MFM,
7.83 MHz CPU). The emulator tracks:

- **Stream position**: where the disk has rotated to (advances with CPU cycles)
- **Read position**: where the CPU has read to (advances on DATA register reads)
- **RDDATA flag**: set when stream position > read position (unread bytes)

This dual-pointer model accurately simulates the SWIM's serial latch behavior.

### Device Manager Intercepts

For higher-level disk I/O, the emulator intercepts the .SONY driver at specific
ROM addresses. When the ROM issues a _Read trap:

1. Intercept at $40315C (`read16_dm_read`) detects the ioRefNum (-5 = floppy)
2. Reads the requested byte count and buffer address from the parameter block
3. Copies data directly from the flash disk image to Mac RAM
4. Sets ioResult = 0 (noErr) and returns

This bypasses the low-level sector read loop for bulk data transfers (loading
the System file, resources, etc.), which is much faster than emulating every
byte of MFM data through the ISM registers.

---

## 11. SCC Z8530 Serial Emulation

The SCC is a stub that returns safe defaults. Two behaviors are critical:

### Register Pointer Mechanism

The Z8530 uses indirect addressing. Writing to WR0 sets a register pointer;
the next read/write accesses that register, then the pointer auto-resets to 0.

### Default Values That Prevent Hangs

- **RR0 = $7C**: Tx Buffer Empty + DCD inactive + CTS inactive + Sync/Hunt.
  Without DCD=1, the ROM thinks a modem carrier is detected and polls forever.
  Without Sync/Hunt (bit 4), the LocalTalk INIT loops checking `(RR0 & $30)==$30`.
- **RR1 = $07**: All Sent — prevents Tx stall detection.

The ROM sends `*APPLE*` on channel A during boot. The emulator does NOT inject
a response — injecting `*S` enters service mode (another infinite loop). The
dispatch loop escape bypass handles this.

---

## 12. Video Pipeline

### Mac Display Format

- 512 x 342 pixels, 1 bit per pixel (monochrome)
- 1 = black, 0 = white (inverted from typical conventions)
- Bit 7 = leftmost pixel, bit 0 = rightmost
- 64 bytes per scanline, 21,888 bytes total
- Located at ScrnBase (read from low-memory global $0824, typically $1FA700)

### Conversion: 1-bit → RGB565

The emulator scales 512x342 to 480x320 using nearest-neighbor sampling with
precomputed lookup tables:

```c
// For each output pixel (dx, dy):
int src_y = y_src_table[dy];           // Precomputed: dy * 342 / 320
int src_byte = x_byte_table[dx];       // Precomputed: (dx * 512/480) / 8
int src_bit = x_bit_table[dx];         // Precomputed: (dx * 512/480) % 8

uint8_t fb_byte = screen[src_y * 64 + src_byte];
uint16_t color = byte_lut[fb_byte][src_bit];  // 256-entry LUT, 8 pixels each
```

The `byte_lut[256][8]` table maps each possible framebuffer byte to 8 RGB565
pixel values. This avoids per-pixel bit shifting.

### Chunked DMA Rendering

The full frame (480x320x2 = 307,200 bytes) cannot fit in a single DMA buffer.
Instead, the frame is rendered in 20-line chunks:

1. Convert 20 lines from 1-bit to RGB565 into a 19,200-byte DMA buffer
2. Call `display_draw()` → `esp_lcd_panel_draw_bitmap()` → SPI DMA transfer
3. Block on semaphore until DMA completes
4. Reuse same buffer for next chunk

The DMA buffer lives in internal SRAM (`MALLOC_CAP_DMA`) because the ESP32's
SPI DMA cannot access PSRAM.

**Performance**: The video task runs at ~10 fps, limited by the SPI transfer
rate (40 MHz) and the 16 chunk transfers per frame.

---

## 13. Touch Input — Trackpad-Style Mouse

The FT5x06 capacitive touch panel acts as a **relative trackpad** (not absolute
positioning) with **tap-to-click** gesture detection. Absolute positioning was
tried first but caused the cursor to jump to random positions because the touch
panel and Mac screen have different coordinate systems and resolutions.

### Trackpad Behavior (Gesture Detection)

- **Swipe/drag** (finger moves >8px): Moves cursor by finger delta. NO button
  press. This lets the user move the cursor without accidentally clicking.
- **Quick tap** (<300ms touch, <8px movement): Generates a click (mouseDown +
  mouseUp). The tap-vs-drag threshold prevents accidental clicks during movement.
- **Tap-then-drag** (second touch within 400ms of a tap release): Starts a
  click-and-drag. The button is held down during the drag, enabling menu selection,
  window dragging, and icon selection.
- **Lift finger**: Stops cursor movement, ends any drag.

### Coordinate Mapping

The FT5x06 reports coordinates in its own frame. The touch panel is physically
rotated relative to the display, so axes are swapped and one axis is inverted:

```c
int dx = -(ty - last_ty);   // Touch Y → Mac horizontal (inverted)
int dy = tx - last_tx;      // Touch X → Mac vertical
mouse_x += dx;
mouse_y += dy;
```

### Mouse Button — Three Layers

On the Mac Classic (an ADB-based Mac), mouse clicks require posting events to
the OS event queue. Setting low-memory globals alone is NOT sufficient. Three
layers work together:

```c
// Layer 1: Event queue posting (ESSENTIAL for clicks)
// Posts mouseDown/mouseUp events to EvQHdr at $014A
mac_post_mouse_event(ram, code, mx, my);

// Layer 2: MBState software mirror (for _Button trap and polling code)
ram[0x0172] = button ? (ram[0x0172] & ~0x80) : (ram[0x0172] | 0x80);

// Layer 3: VIA PB3 hardware button (active low)
via_set_mouse_button(pressed);  // Sets/clears VIA IRB bit 3
```

**Why three layers?** The Mac Classic uses ADB for mouse input. The ADB completion
routine normally reads mouse data, updates MBState, AND posts mouseDown/mouseUp
events to the OS event queue. Since ADB is fully bypassed (no real ADB hardware),
no handler posts events. Applications call GetNextEvent/WaitNextEvent which reads
from the event queue — NOT from MBState. Without event queue posting, clicks are
invisible to the system.

### Event Queue Posting Implementation

The `mac_post_mouse_event()` function writes EvQEl (event queue element) structures
directly to Mac RAM and links them into the EvQHdr queue at $014A:

```c
// Fixed EvQEl buffers in 68010-reserved vector area (unused on 68000 Mac):
#define EVT_BUF_DOWN   0x00C0   // mouseDown EvQEl (22 bytes)
#define EVT_BUF_UP     0x00D8   // mouseUp EvQEl  (22 bytes)

// EvQEl structure (22 bytes):
// +0:  qLink (4)        — next element pointer (0 = last)
// +4:  qType (2)        — 5 (evType)
// +6:  evtQWhat (2)     — 1=mouseDown, 2=mouseUp
// +8:  evtQMessage (4)  — 0 for mouse events
// +12: evtQWhen (4)     — Ticks value from $016A
// +16: evtQWhere (4)    — mouse Point (V.hi V.lo H.hi H.lo)
// +20: evtQModifiers (2)— 0
```

Before posting, `evt_in_queue()` walks the active queue to verify the buffer
isn't still in use from a previous event. After the Mac's GetNextEvent dequeues
the element, it enters the Event Manager's free pool — this is harmless.

### Writing Cursor Position to Mac Low-Memory Globals

Once per frame (after VBlank, gated by `mac_post_boot` + WMgrPort valid), the
emulator writes cursor position to Mac RAM — **only when position changes**:

```c
if (pos_changed) {
    // Mac Point format: Y.hi, Y.lo, X.hi, X.lo (big-endian)
    ram[0x0828..0x082B] = mouse_y, mouse_x;              // MTemp
    ram[0x082C..0x082F] = same;                           // RawMouse
    ram[0x0830..0x0833] = same;                           // Mouse
    ram[0x08CE] = 0xFF;                                   // CrsrNew (trigger redraw)
    ram[0x08CF] = 0xFF;                                   // CrsrCouple
}
```

**CRITICAL: CrsrNew crash bug.** Writing `CrsrNew=$FF` unconditionally every
frame forces cursor redraws that crash the resource decompressor (DCMP) during
INIT/Finder loading. The decompressor state gets corrupted by the cursor redraw
interrupt, causing a wild PC jump to unmapped memory. The fix:
1. Only set CrsrNew when mouse position actually changes from last-written value
2. Initialize `prev_mx`/`prev_my` to `MAC_SCREEN_WIDTH/2`/`MAC_SCREEN_HEIGHT/2`
   (matching `mouse_x`/`mouse_y` defaults) to prevent a false delta on the first
   frame when WMgrPort becomes valid

---

## 14. Performance Optimization

### The Bottleneck

Profiling with `esp_cpu_get_cycle_count()` revealed that **97% of frame time
is spent inside `m68k_execute()`** — the Musashi CPU emulation loop. Only 3%
is overhead (VIA tick, address decode, interrupt checks, etc.).

### M68K_SEPARATE_READS (17% Speedup)

The biggest single optimization. Musashi's `M68K_SEPARATE_READS` option splits
instruction fetches from data reads into separate callback functions:

**Before (OPT_OFF):** All memory reads — instruction fetches and data reads —
go through the same `m68k_read_memory_16()` function, which includes all
diagnostic intercepts (watchpoints, boot bypasses, trap logging).

**After (OPT_ON):** Instruction fetches call `m68k_read_immediate_16()`, which
has a lean "fast path" that skips all diagnostic intercepts:

```c
IRAM_ATTR unsigned int m68k_read_immediate_16(unsigned int address)
{
    address &= 0x00FFFFFF;

    // During boot: full path with all intercepts
    if (!mac_post_boot) return m68k_read_memory_16(address);

    // Post-boot: lean RAM path (most instructions)
    if (address < 0x400000) {
        uint32_t ra = address & ram_mask;
        return (ram[ra] << 8) | ram[ra + 1];
    }

    // Post-boot: lean ROM path (with essential patches only)
    if (address < 0x500000) {
        // Only 8 intercept checks vs. dozens in the full path
        ...
        return (rom[address & rom_mask] << 8) | rom[(address & rom_mask) + 1];
    }

    return m68k_read_memory_16(address);
}
```

Key details:
- All 5 lean functions are marked `IRAM_ATTR` (placed in internal SRAM, 728
  bytes total, 70 KB headroom). This bypasses the flash instruction cache.
- `__builtin_expect()` hints guide branch prediction (RAM path is the common case).
- `mac_post_boot` flag is set at frame 2000 (after Finder launches).
- During boot, the lean functions fall through to the full path, preserving
  all diagnostic intercepts.

### Other Optimizations

- **PC consolidation**: Single `m68k_get_reg(PC)` per slice shared by all
  post-execute checks (was 8 separate calls).
- **Disabled Musashi features**: Prefetch emulation OFF (saves a branch per
  instruction), address error emulation OFF (saves setjmp per execute call).
- **PSRAM for Musashi tables**: The 580 KB opcode/cycle tables live in PSRAM
  via `EXT_RAM_BSS_ATTR`, freeing internal SRAM for fast-access code.
- **Disassembler excluded**: `m68kdasm.c` is not compiled, saving ~256 KB.

### What Was Tried and Didn't Work

- **fast_slice > 4000**: Crashes deterministically. VIA T2 periods are 100-2000
  cycles; larger slices miss timer underflows. Tested exhaustively up to 40000
  with timer arm tracking — unsolvable without running via_tick inside Musashi.
- **Mid-slice VIA arm tracking**: Recording cycle counts at T1CH/T2CH writes,
  adjusting via_tick — no effect on crash.
- **m68k_end_timeslice()**: Breaks boot entirely.
- **Event prediction**: Setting cycle counter to stop at next timer event —
  breaks boot.

### Performance Numbers

| Metric | Value |
|--------|-------|
| Steady state (idle loop) | 85-91% real-time |
| Peak | 91% |
| During boot/loading | 39-67% |
| Range | 39-91% |
| Video | ~10 fps |
| Exec vs overhead | 96-97% / 3-4% |
| fast_slice | 4000 cycles (MUST NOT exceed) |

---

## 15. Emulation Loop — Frame by Frame

Each iteration of `mac_emu_run()` executes one frame (~130,560 CPU cycles):

```
┌─────────────────────────────────────────────────────────┐
│  Frame Start                                            │
│  ├── Record cycle count for profiling                   │
│  │                                                      │
│  ├── Inner loop: execute CPU in slices until budget done│
│  │   ├── Choose slice size:                             │
│  │   │   F0-80: 1 cycle (memory test bypass)            │
│  │   │   F600-640: 1 cycle (trap ring buffer)           │
│  │   │   Disk active: 10 cycles (driver intercepts)     │
│  │   │   F700-2000: 1000 cycles (normal boot)           │
│  │   │   F2000+: 4000 cycles (max throughput)           │
│  │   │                                                  │
│  │   ├── m68k_execute(slice)  ← 97% of time here       │
│  │   ├── via_tick(executed)                              │
│  │   ├── Check/raise VIA IRQ                            │
│  │   ├── PC validation (crash detection + recovery)     │
│  │   ├── VBlank at mid-frame                            │
│  │   └── Boot bypass checks (PC-specific)               │
│  │                                                      │
│  ├── Frame end:                                         │
│  │   ├── Increment frame_count                          │
│  │   ├── Profiling accumulation                         │
│  │   ├── Fire VBlank if not already fired               │
│  │   ├── One-second interrupt every 60 frames           │
│  │   ├── Speed report every 300 frames                  │
│  │   │                                                  │
│  │   ├── Write mouse state to Mac low-memory globals    │
│  │   │   (MTemp, RawMouse, Mouse, CrsrNew, MBState)    │
│  │   │   + Post mouseDown/mouseUp to event queue $014A  │
│  │   │                                                  │
│  │   └── Poll touch input (~60 Hz)                      │
│  │       └── Gesture detection: tap/drag/tap-then-drag  │
│  │                                                      │
│  └── taskYIELD() — prevent watchdog timeout              │
└─────────────────────────────────────────────────────────┘
```

---

## 16. PSRAM Budget

| Allocation | Size | Type | Notes |
|------------|------|------|-------|
| Musashi opcode table | ~320 KB | BSS (EXT_RAM_BSS_ATTR) | Static, set once at init |
| Musashi cycle table | ~260 KB | BSS (EXT_RAM_BSS_ATTR) | Static, set once at init |
| Mac RAM | 2,048 KB | Heap (SPIRAM) | Main emulated memory |
| **Total** | **~2,628 KB** | | |
| **Headroom** | **~1,468 KB** | | |

Note: The disk image (1.44 MB) is accessed directly from flash (.rodata), not
copied to PSRAM. This saves PSRAM for Mac RAM.

### Why 2 MB Mac RAM?

- Must be a power of 2 for correct ROM hardware-style aliasing (ram_mask = ram_size - 1)
- 2 MB is a valid Mac Classic configuration (1 MB base + 1 MB SIMM)
- 4 MB would leave no PSRAM headroom for Musashi tables
- System 7.5.3 runs adequately in 2 MB

---

## 17. Build System

### ESP-IDF v6.0

The project uses ESP-IDF v6.0 (`C:\esp\v6.0\esp-idf`). **You must build via
`cmd.exe`** — ESP-IDF v6.0 rejects MSYS/MinGW shells.

```bash
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat build"
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat flash"
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat monitor"
```

### ROM and Disk Image Embedding

The ROM and disk image are embedded in the firmware binary via CMake `EMBED_FILES`.
The linker generates symbols like `_binary_Classic_ROM_start` that C code references.
The disk image is accessed directly from flash (read-only) to save PSRAM.

### Partition Table

| Name | Type | Size |
|------|------|------|
| nvs | data | 24 KB |
| phy_init | data | 4 KB |
| factory | app | ~3,968 KB |

The factory partition holds firmware (~600 KB code) + ROM (512 KB) + disk (1,440 KB),
totaling ~2.5 MB with ~1.5 MB free.

---

## 18. Challenges Solved — Debugging History

### Phase 1: Black Screen → Overlay Fix
Overlay toggled on every ORA write → ROM/RAM addresses scrambled.
**Fix**: One-shot latch (first ORA write clears overlay permanently).

### Phase 2: Memory Test Fails → D6 Bypass
ROM memory test found "errors" because overlay was still wrong.
**Fix**: Force D6=0 at three checkpoint PCs (now just a safety net).

### Phase 3: Infinite Dispatch Loop → PC Redirect
ROM's serial debug monitor has NO exit path.
**Fix**: Redirect PC to ROM's own MOVEM/RTS cleanup at $401F2C.

### Phase 4: IWM Busy-Wait → Flag Clearing
IWM stub couldn't complete sector reads.
**Fix**: Clear busy flag in RAM after timeout (later replaced by full ISM emulation).

### Phase 5: SCSI Stuck Bus → Return $00
SCSI reads returned $FF (all lines asserted).
**Fix**: Return $00 (bus free, no devices).

### Phase 6: "?" Floppy Icon → Working!
ROM fully booted to the "?" icon (no disk). First milestone.

### Phase 7: Disk I/O → SWIM ISM Emulation
Implemented full MFM sector read emulation through ISM registers (DATA, MARK,
CRC, HANDSHAKE). Byte clock model with dual stream/read pointers. Device Manager
intercepts for bulk reads.

### Phase 8: Boot Stalls → ADB, LocalTalk, StartUpScreen
Multiple INITs stalled waiting for hardware:
- ADB bus idle (VIA PB4+PB5 force high)
- VIA SR interrupts for ADB protocol
- SCC Sync/Hunt bit for LocalTalk
- StartUpScreen resource skip
- UNIMP trap handler for unimplemented Toolbox calls

### Phase 9: Finder Launches → Success!
System 7.5.3 Finder running, files visible, menus work.

### Phase 10: Performance → 75% Real-Time
M68K_SEPARATE_READS + IRAM lean functions gave 17% speedup (62% → 75%).
fast_slice > 4000 confirmed impossible.

### Phase 11: Touch Input → Trackpad Mouse
Absolute positioning caused cursor jumping. Changed to relative trackpad-style
movement (delta-based). Writes to MTemp, RawMouse, Mouse, CrsrNew, MBState.

### Phase 12: CrsrNew Crash Fix + VIA Mouse Button
Unconditional `CrsrNew=$FF` every frame crashed the resource decompressor (DCMP)
during INIT/Finder loading. Fixed by only writing CrsrNew on actual position change,
with initial tracking values matching mouse defaults. Separately, mouse clicks didn't
register because the Mac reads button state from VIA Port B bit 3 (PB3, active low),
not from MBState. Fixed by adding `via_set_mouse_button()` to update VIA IRB PB3,
and initializing IRB with PB3=1 ($39) for button-up default.

### Phase 13: Event Queue Posting + Tap-vs-Drag
Mouse clicks via MBState and VIA PB3 alone did NOT produce visible clicks in the
Mac UI. Root cause: on the Mac Classic (ADB-based), the event system (GetNextEvent/
WaitNextEvent) reads mouseDown/mouseUp events from the OS event queue at $014A, NOT
from MBState. Since ADB is bypassed, no handler posts events. Fixed by implementing
`mac_post_mouse_event()` which writes EvQEl structures directly to RAM ($00C0/$00D8)
and links them into the event queue. Also replaced always-button-down-on-touch with
proper gesture detection: swipe = move cursor (no click), tap = click, tap-then-drag
= click-and-drag.

---

## 19. Current Status and Future Work

### Working (April 2026)

- Full boot: ROM self-test → boot block → System file → INITs → Finder
- System 7.5.3 desktop with icons, menus, windows
- **Working mouse clicks** via event queue posting to $014A
- Touch-screen trackpad with gesture detection (tap to click, swipe to move, tap-then-drag)
- Three-layer mouse: event queue + MBState + VIA PB3
- Peak 91% real-time speed (39-91% range)
- ~10 fps video output
- VIA timers and interrupts
- Full SWIM ISM floppy emulation
- Crash detection and recovery

### Future Work

- **Further performance**: 97% of time is in m68k_execute(). Gains require
  optimizing Musashi core (computed goto dispatch, cycle table in IRAM),
  reducing PSRAM access overhead, or 68030 upgrade.
- **68030 upgrade**: Use M68K_CPU_TYPE_68030 + Color Classic ROM for faster
  instruction execution (fewer cycles per instruction).
- **Keyboard input**: Touch-screen virtual keyboard via VIA shift register.
- **Virtual SCSI hard drive**: PSRAM or flash partition for OS installation.
- **SCC serial emulation**: Real serial port passthrough via ESP32 UART.
- **Sound output**: I2S or PWM DAC from the Mac sound buffer.

---

## 20. Glossary

| Term | Meaning |
|------|---------|
| 68000 / 68K | Motorola 68000 CPU (16/32-bit CISC, 24-bit address bus) |
| A-line trap | Opcode $Axxx — triggers Line-A exception, used for Mac OS system calls |
| ADB | Apple Desktop Bus — protocol for keyboard/mouse on later Macs |
| ACR | Auxiliary Control Register (VIA) — selects timer and shift register modes |
| DMA | Direct Memory Access — hardware transfers data without CPU involvement |
| DRAM | Dynamic RAM (on ESP32: internal static RAM confusingly called DRAM) |
| ESP-IDF | Espressif IoT Development Framework — FreeRTOS-based SDK for ESP32 |
| GCR | Group Code Recording — 400K/800K Mac floppy encoding format |
| IER | Interrupt Enable Register (VIA) — masks which interrupts can fire |
| IFR | Interrupt Flag Register (VIA) — shows which interrupts are pending |
| IRAM | Instruction RAM — fast internal SRAM on ESP32, bypasses flash cache |
| ISM | Integrated SWIM Module — SWIM's native MFM register interface |
| IWM | Integrated Woz Machine — original Mac floppy controller (GCR only) |
| MFM | Modified Frequency Modulation — 1.44 MB floppy encoding format |
| Musashi | Open-source 68000 CPU emulator by Karl Stenerud |
| NCR 5380 | SCSI bus controller chip |
| ORA/ORB | Output Register A/B (VIA) — port output latches |
| Overlay | ROM/RAM address map swap controlled by VIA, active at reset |
| PSRAM | Pseudo-Static RAM — external SPI RAM on ESP32-WROVER modules |
| ROM | Read-Only Memory — the Mac Classic's 512 KB firmware |
| SCC | Serial Communications Controller — Zilog Z8530 dual-channel UART |
| SR | Shift Register (VIA) — serial data for ADB/keyboard protocol |
| SWIM | SuperDrive Woz Integrated Machine — floppy controller for MFM disks |
| VIA | Versatile Interface Adapter — MOS 6522, central system I/O chip |
| VBlank | Vertical Blank — ~60 Hz interrupt when display finishes a frame |
