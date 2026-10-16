# Macintosh Classic Emulator for ESP32 -- Architecture Document

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Hardware Platform](#2-hardware-platform)
3. [Emulated Hardware](#3-emulated-hardware)
4. [Memory Map](#4-memory-map)
5. [Boot Sequence and ROM Bypasses](#5-boot-sequence-and-rom-bypasses)
6. [VIA 6522 Emulation](#6-via-6522-emulation)
7. [Video Pipeline](#7-video-pipeline)
8. [SCC Z8530 Emulation](#8-scc-z8530-emulation)
9. [IWM/SWIM Floppy Emulation](#9-iwmswim-floppy-emulation)
10. [Challenges Solved](#10-challenges-solved)
11. [PSRAM Budget](#11-psram-budget)
12. [Build System](#12-build-system)
13. [Current Status and Future Work](#13-current-status-and-future-work)

---

## 1. Project Overview

This project is a Macintosh Classic emulator running on an ESP32 microcontroller
(WT32-SC01-3.4 board). It emulates a Motorola 68000 CPU at 7.8336 MHz using the
Musashi CPU emulation library, along with the minimum set of Macintosh Classic
peripherals required to boot the original ROM:

- **VIA 6522** -- system control, interrupts, keyboard, overlay
- **SCC Z8530** -- serial controller (stub)
- **IWM/SWIM** -- floppy disk controller (stub)
- **SCSI NCR 5380** -- hard disk controller (stub)
- **Video** -- 512x342 monochrome framebuffer

The Mac's 1-bit 512x342 monochrome display output is scaled to 480x320 RGB565
and rendered on the board's SPI-connected ST7796 LCD via DMA transfers. Touch
input from the FT5x06 capacitive digitizer is mapped to Mac mouse coordinates.

### Software Architecture

```
+----------------------------------------------------------+
|                     main.cpp                              |
|  app_main() -- display_init, mac_emu_init, task create    |
+------------------+---------------------------------------+
                   |
                   v
+----------------------------------------------------------+
|                  mac_emulator.c                           |
|  mac_emu_run() -- main loop: CPU execute, VIA tick,      |
|                   VBlank, render, touch, boot bypasses    |
+--------+---------+---------+---------+-------------------+
         |         |         |         |
         v         v         v         v
   mac_memory.c  mac_via.c  mac_scc.c  mac_iwm.c
   (bus decode)  (6522)     (Z8530)    (IWM/SWIM)
         |
         v
   Musashi m68k  <-- m68k_read/write_memory_* callbacks
   (m68kcpu.c, m68kops.c)
```

### File Layout

| File | Purpose |
|------|---------|
| `main/main.cpp` | ESP-IDF entry point (`app_main`), display init, task creation |
| `main/mac_emulator.c` | Main emulation loop, frame timing, boot bypasses |
| `main/mac_emulator.h` | Mac Classic constants (CPU clock, screen size, timing) |
| `main/mac_memory.c` | 24-bit address bus decode, Musashi memory callbacks |
| `main/mac_memory.h` | Memory map documentation, init/access API |
| `main/mac_via.c` | VIA 6522 emulation (ports, timers, interrupts, overlay) |
| `main/mac_via.h` | VIA register definitions, IRQ bit masks |
| `main/mac_scc.c` | SCC Z8530 stub (register pointer, safe defaults) |
| `main/mac_scc.h` | SCC API |
| `main/mac_iwm.c` | IWM/SWIM stub (sense lines, no sector reads) |
| `main/mac_iwm.h` | IWM API |
| `main/mac_video.c` | 1-bit to RGB565 converter, chunked DMA renderer |
| `main/mac_video.h` | Video API |
| `components/musashi/` | Musashi 68000 CPU emulator (third-party, copied) |
| `components/display/` | Native esp_lcd display driver (ST7796 + FT5x06) |

---

## 2. Hardware Platform

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

### SoC: ESP32-D0WD rev v1.0

- Dual-core Xtensa LX6 @ 240 MHz
- **Flash**: 4 MB (80 MHz DIO) -- note: board silkscreen says "16MB" but
  only 4 MB is detected. The single `factory` partition uses nearly all of it
  (~3.9 MB) to hold firmware + embedded ROM + embedded disk image.
- **PSRAM**: 8 MB physical, **~4 MB usable** due to ESP32 virtual address
  space limitation (the hardware can only map ~4 MB of PSRAM into the CPU's
  address space at once via the SPI cache window).
- Internal SRAM: ~365 KB free, ~302 KB DMA-capable

### Display: ST7796 SPI LCD (480x320)

| Signal | GPIO |
|--------|------|
| MOSI | 13 |
| CLK | 14 |
| CS | 15 |
| DC | 21 |
| RST | 22 |
| Backlight | 23 |

- SPI frequency: 40 MHz
- Color format: RGB565 (16-bit, big-endian over SPI)
- The ESP-IDF `esp_lcd_panel_st7789` driver is used (ST7796 is
  register-compatible with ST7789).
- Panel is configured in landscape mode via `swap_xy(true)` and
  `mirror(false, true)`.

### Touch: FT5x06 Capacitive I2C

| Signal | GPIO |
|--------|------|
| SDA | 18 |
| SCL | 19 |

- I2C address: 0x38
- Bus speed: 400 kHz
- The raw touch coordinates are read directly from FT5x06 registers (no
  managed component). Touch failure is non-fatal -- the emulator runs without
  mouse input if the touch controller is not responsive.

---

## 3. Emulated Hardware

The Macintosh Classic is a relatively simple system built around the Motorola
68000 CPU. This emulator implements the minimum peripheral set required to boot
the Classic ROM.

### Motorola 68000 CPU

- Emulated by the **Musashi** library (Karl Stenerud), version supporting
  68000/68010/68020/68030/68040.
- Configured as `M68K_CPU_TYPE_68000` (24-bit address bus, no cache, no MMU).
- Original clock: 7.8336 MHz. The emulator runs cycles per frame rather than
  matching real-time clock accuracy.
- **~130,560 cycles per VBlank frame** (7,833,600 Hz / 60 Hz).
- Musashi calls back into `mac_memory.c` for all bus access via
  `m68k_read_memory_{8,16,32}` and `m68k_write_memory_{8,16,32}`.
- Interrupts use autovectoring (`M68K_INT_ACK_AUTOVECTOR`).
- The Musashi disassembler (`m68kdasm.c`) is **excluded** from the build to
  save ~256 KB of DRAM.
- Musashi's large opcode jump table and cycle count tables (~580 KB total) are
  placed in PSRAM via `EXT_RAM_BSS_ATTR` -- they cannot fit in the ~180 KB of
  available internal DRAM.

### VIA 6522

The single VIA chip in the Mac Classic is the central system control hub. See
[Section 6](#6-via-6522-emulation) for full details.

### SCC Z8530

The Zilog 8530 Serial Communications Controller provides two serial channels
(A = modem port, B = printer port). In this emulator it is a stub returning
safe defaults. See [Section 8](#8-scc-z8530-emulation).

### IWM/SWIM

The Integrated Woz Machine (or its successor SWIM) controls the 3.5" floppy
drive. This emulator provides sense-line responses but cannot complete sector
reads. See [Section 9](#9-iwmswim-floppy-emulation).

### SCSI NCR 5380

The Mac Classic has an internal SCSI bus for hard drives. The emulator returns
`0x00` for all SCSI register reads (address range `0x580000-0x5FFFFF`),
indicating "bus free, no devices." This is a critical stub -- returning `0xFF`
(the default for unmapped space) would signal all SCSI bus lines asserted, which
the ROM interprets as a stuck bus and enters error handling.

---

## 4. Memory Map

The Macintosh Classic uses a 24-bit address bus (16 MB address space). Address
decoding is handled by a PAL chip on the real hardware; in the emulator it is
implemented as cascading if-else checks in `mac_memory.c:mem_read_byte()` and
`mem_write_byte()`.

### Normal Mode (Overlay = OFF)

This is the standard operating mode after boot initialization completes.

```
24-bit Address Space (16 MB)
+==========================+ 0x000000
|                          |
|       RAM (2 MB)         |  Mirrored to fill 4 MB region
|  (physical: 0-1FFFFF)   |  via ram_mask = ram_size - 1
|                          |
+--------------------------+ 0x400000
|       ROM (512 KB)       |  Mirrored within 1 MB region
|  (physical: 0-7FFFF)    |  via rom_mask = rom_size - 1
+--------------------------+ 0x500000
|    Phase-adjusted RAM    |  (not implemented)
+--------------------------+ 0x580000
|     SCSI NCR 5380        |  Returns 0x00 (bus free)
+--------------------------+ 0x600000
|       Unused             |
+--------------------------+ 0x800000
|     SCC Read             |  Zilog 8530 control/data read
+--------------------------+ 0xA00000
|     SCC Write            |  Zilog 8530 control/data write
+--------------------------+ 0xC00000
|     IWM / SWIM           |  Floppy disk controller
+--------------------------+ 0xE00000
|       VIA 6522           |  System control, timers, interrupts
+--------------------------+ 0xF00000
|       Unused             |
+==========================+ 0xFFFFFF
```

### Overlay Mode (Overlay = ON)

At power-on/reset, the VIA overlay flip-flop is SET, remapping ROM to address 0
so the 68000 can fetch its reset vectors (SSP at 0x000000, PC at 0x000004).

```
+==========================+ 0x000000
|       ROM (512 KB)       |  ROM mapped here for reset vectors
|  (mirrored to fill 4MB)  |
+--------------------------+ 0x400000
|       ROM (normal loc)   |  Still accessible at normal address
+--------------------------+ 0x500000
|       ...                |
+--------------------------+ 0x600000
|       RAM                |  RAM accessible at alternate address
|  (0x600000 - 0x7FFFFF)  |  during overlay for vector copy
+--------------------------+ 0x800000
|  (peripherals same)      |
+==========================+ 0xFFFFFF
```

### Overlay Write-Through Behavior

A crucial detail: writes to address `0x000000-0x3FFFFF` during overlay mode go
to RAM, not ROM. This is because the overlay PAL only controls the read chip
select -- ROM is physically read-only, so the RAM chip always receives writes
regardless of overlay state. The ROM boot code exploits this to copy exception
vectors from ROM to RAM at address 0 while overlay is still active:

1. ROM reads its own vector table from 0x000000 (overlay maps ROM here)
2. ROM writes vectors to 0x000000 (writes go to RAM regardless of overlay)
3. ROM clears overlay (first ORA write to VIA)
4. Now address 0x000000 reads from RAM, which contains the copied vectors

### Post-Overlay Vector Table Exception

After overlay is cleared for the first time, a flag (`overlay_was_off`) is set
permanently. If overlay is ever re-enabled (which can happen during certain ROM
operations), the vector table region (`0x0000-0x03FF`) is read from **RAM**
instead of ROM. This prevents exception handlers from hitting ROM boot code
instead of the installed interrupt vectors.

### RAM Layout (2 MB)

```
0x000000 +------------------+
         | Exception Vectors |  0x000 - 0x3FF (1 KB)
         +------------------+
         | System Globals    |  Low memory globals used by ROM/OS
         +------------------+
         |                   |
         |   Application     |
         |   Memory          |
         |                   |
         +------------------+
         | Screen Buffer     |  RAM_SIZE - 0x5900 = 0x1FA700
         | (21,888 bytes)    |  512x342 pixels, 1 bit/pixel
         +------------------+ 0x1FFD00
         | Sound Buffer      |  RAM_SIZE - 0x0300
         | (768 bytes)       |
         +------------------+ 0x200000
```

Key low-memory globals referenced by the emulator's diagnostic logging:

| Address | Name | Purpose |
|---------|------|---------|
| `0x0824` | ScrnBase | Pointer to screen framebuffer base |
| `0x0108` | MemTop | Top of usable memory |

---

## 5. Boot Sequence and ROM Bypasses

The Mac Classic ROM performs extensive hardware initialization before reaching
the desktop. Several steps assume real hardware that the emulator cannot fully
provide, requiring targeted bypasses. Each bypass addresses a specific failure
mode discovered during the debugging process.

### 5.1 Normal Boot Flow

```
Power On / Reset
       |
       v
  68000 reads SSP from 0x000000 (ROM via overlay)
  68000 reads PC  from 0x000004 (ROM via overlay)
       |
       v
  ROM entry point (in 0x400xxx range)
       |
       v
  Copy exception vectors from ROM to RAM
  (writes to 0x000000 go to RAM through overlay)
       |
       v
  First ORA write to VIA --> clears overlay
  (one-shot latch: overlay stays OFF permanently)
       |
       v
  Memory test (multiple stages)
  Tests RAM, stores results in D6
       |
       v
  Check PA0 on VIA Port A
  (PA0=0 means diagnostic device attached --> enter serial debug monitor)
  (PA0=1 means no diagnostic device --> continue normal boot)
       |
       v
  SCC initialization
  ROM sends "*APPLE*" on channel A, listens for "*S" response
  (dispatch loop at 0x401F38 processes SCC traffic)
       |
       v
  IWM/disk driver initialization
  Attempts to read boot blocks from floppy
       |
       v
  Display "?" floppy icon or boot from disk
```

### 5.2 Bypass: VIA Overlay One-Shot Latch

**What it does**: The first write to VIA ORA (Output Register A) permanently
clears the overlay flip-flop. Subsequent ORA writes do not re-enable overlay.

**Why it exists**: The real Macintosh uses a one-shot latch (74LS393 counter
configured as a flip-flop) for the overlay signal. Once the ROM writes ORA for
the first time -- after copying vectors to RAM -- the latch clears and stays
clear. The ROM later writes ORA again for other purposes (sound volume, screen
buffer select, SCC wait control), and these writes must NOT re-enable overlay.

**The initial bug**: The emulator originally tracked overlay as bit 4 of ORA,
toggling it on every ORA write. This caused overlay to flip back and forth,
corrupting the address map after boot and causing memory test failures. The fix
was to implement the one-shot latch: `via.overlay` starts `true` and is
cleared on the first ORA write, then never set again.

**Implementation** (`mac_via.c`, ORA write handler):
```c
if (via.overlay) {
    via.overlay = false;
    via.overlay_was_off = true;
}
```

### 5.3 Bypass: Memory Test D6 Clearing

**What it does**: At three specific PC addresses during early boot
(`0x401CF0`, `0x401D80`, `0x401D96`), if register D6 is nonzero, force it to
zero.

**Why it exists**: The ROM's memory test stores error flags in D6. If D6 != 0
after the test, the ROM concludes memory is faulty and either retries or enters
the dispatch loop (serial debug monitor) which has no exit path. With correct
overlay emulation, the memory tests now pass cleanly and D6 is already zero at
these checkpoints. The bypass is retained as a **safety net** -- it logs when
triggered but no longer fires under normal operation.

**The root cause**: Before the overlay one-shot fix, the ROM's memory test
would write test patterns to RAM via address 0x000000, then read them back.
With inverted overlay logic, reads would intermittently hit ROM instead of RAM,
causing test pattern mismatches.

**Implementation** (`mac_emulator.c`, inside the per-slice execution loop):
```c
if (frame_count < 80) {
    uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
    if (pc == 0x401CF0 || pc == 0x401D80 || pc == 0x401D96) {
        uint32_t old_d6 = m68k_get_reg(NULL, M68K_REG_D6);
        if (old_d6 != 0) {
            m68k_set_reg(M68K_REG_D6, 0);
        }
    }
}
```

### 5.4 Bypass: Dispatch Loop Escape

**What it does**: After detecting the PC in range `0x401F38-0x402500` for more
than 5 consecutive frames, redirects PC to `0x401F2C` (the ROM's own register
restore and RTS sequence).

**Why it exists**: The dispatch loop at `0x401F38` is the Mac's built-in serial
debug monitor. It is entered via the service dispatch routine at `0x401DB0`,
which saves all registers (D0-D7, A0-A6, SR) to the stack using MOVEM. The
dispatch loop reads SCC data, processes debugger commands, and branches back to
itself. **There is no timeout and no exit path** -- even the "correct" response
(`*S` handshake) causes a `BRA.W` back into the loop at `0x401F4A`.

The bypass leverages the ROM's own cleanup code at `0x401F2C`:
```
0x401F2C: MOVE.W (SP)+, SR       ; Restore status register
0x401F2E: MOVEM.L (SP)+, D0-D7/A0-A6  ; Restore all registers
0x401F32: RTS                     ; Return to calling init subroutine
```

By redirecting PC here, all saved registers are properly restored from the
stack, and execution returns to the code that originally called the service
dispatch, letting boot continue normally.

**Why not inject the `*S` handshake?** Injecting `*S` into the SCC receive
buffer was tried and abandoned. The `*S` response puts the ROM into "service
mode" where it waits for further debugger commands -- another infinite loop.
The only clean exit is to skip the dispatch entirely.

### 5.5 Bypass: IWM Busy-Flag Clearing

**What it does**: When the PC is stuck in the IWM disk driver's busy-wait loop
(`0x40335E-0x403364`) for more than 3 consecutive frames, clears bit 5 of the
RAM byte at `(A3 + 0x015D)`.

**Why it exists**: The ROM's disk driver executes:
```
0x40335E: BTST  #5, (A3, $015D)   ; Test busy flag
          BNE.S $40335E            ; Loop while set
```
This loop waits for the IWM hardware to complete a sector read and clear the
busy flag via an interrupt handler. Since the IWM emulator is a stub that
cannot complete disk operations, the interrupt never fires and the busy flag
is never cleared. The bypass manually clears the flag, allowing the ROM to
discover that no valid boot blocks were read and display the "?" floppy icon.

The bypass can fire multiple times as the ROM retries disk reads.

### 5.6 Bypass: SCSI Bus Free Stub

**What it does**: Returns `0x00` for all reads in the SCSI address range
(`0x580000-0x5FFFFF`).

**Why it exists**: The NCR 5380 SCSI controller uses various status lines
(BSY, SEL, REQ, etc.) that appear as register bits. The default return value
for unmapped address space is `0xFF`, which would mean "all signals asserted"
-- the ROM interprets this as a stuck SCSI bus and enters error recovery. By
returning `0x00` ("bus free, no devices"), the ROM's SCSI probe completes
quickly and moves on.

---

## 6. VIA 6522 Emulation

The VIA (Versatile Interface Adapter) is the most important peripheral in the
Mac Classic. It handles system interrupts, keyboard communication, sound
control, overlay management, and more.

### Register Map

VIA registers are at base address `0xEFE1FE` with 512-byte (0x200) spacing.
Register index is extracted as `(address >> 9) & 0xF`.

| Reg | Index | Name | Mac Usage |
|-----|-------|------|-----------|
| ORB | 0 | Output Register B | Keyboard clock/data, RTC |
| ORA | 1 | Output Register A | Overlay, sound volume, screen buf, SCC wait |
| DDRB | 2 | Data Direction B | Port B pin directions |
| DDRA | 3 | Data Direction A | Port A pin directions |
| T1CL | 4 | Timer 1 Count Low | |
| T1CH | 5 | Timer 1 Count High | System timer |
| T1LL | 6 | Timer 1 Latch Low | |
| T1LH | 7 | Timer 1 Latch High | |
| T2CL | 8 | Timer 2 Count Low | |
| T2CH | 9 | Timer 2 Count High | Sound/utility timer |
| SR | 10 | Shift Register | Keyboard serial data |
| ACR | 11 | Auxiliary Control | Timer modes, shift register mode |
| PCR | 12 | Peripheral Control | CA1/CA2/CB1/CB2 edge select |
| IFR | 13 | Interrupt Flag | Pending interrupts |
| IER | 14 | Interrupt Enable | Interrupt mask |
| ORA_NH | 15 | ORA (no handshake) | Same as ORA but no CA1/CA2 clear |

### Port A Bits

| Bit | Signal | Function |
|-----|--------|----------|
| 0 | PA0 | Diagnostic detect (0 = diagnostic device attached) |
| 1 | PA1 | Not used in Classic |
| 2 | PA2 | Not used in Classic |
| 3 | PA3 | /SCC.IRQ (active low: 1 = no SCC interrupt) |
| 4 | PA4 | ROM overlay control (one-shot: first write clears) |
| 5 | PA5 | Alternate screen buffer select |
| 6 | PA6 | Main screen buffer select |
| 7 | PA7 | SCC wait/request |

**Critical initialization**: `IRA` (input register A) is initialized to `0x0F`:
- PA0 = 1 (no diagnostic device) -- if 0, ROM enters serial debug monitor
- PA3 = 1 (no SCC interrupt pending) -- prevents spurious SCC interrupt handling

### Port B Bits

| Bit | Signal | Function |
|-----|--------|----------|
| 0 | PB0 | RTC serial data (bidirectional) |
| 1 | PB1 | RTC serial clock |
| 2 | PB2 | RTC enable |
| 3-7 | | Keyboard and other I/O |

### Interrupt Sources

The VIA generates a level-1 autovectored interrupt to the 68000.

| Bit | Flag | Source | Frequency |
|-----|------|--------|-----------|
| 0 | CA2 | One-second RTC clock | ~1 Hz |
| 1 | CA1 | VBlank | ~60 Hz |
| 2 | SR | Shift register complete | On keyboard transfer |
| 3 | CB2 | Keyboard data ready | On keypress |
| 4 | CB1 | Keyboard clock | On keypress |
| 5 | T2 | Timer 2 underflow | One-shot, on demand |
| 6 | T1 | Timer 1 underflow | Free-running or one-shot |
| 7 | ANY | Set if any enabled IRQ pending | (composite) |

The interrupt dispatch works as follows:
1. VIA sets flag bits in IFR when events occur
2. `via_update_irq()` sets bit 7 if `(IFR & IER & 0x7F) != 0`
3. Main loop checks `via_irq_pending()` and raises/lowers `M68K_IRQ_1`
4. 68000 acknowledges with autovector

### Timer Implementation

**Timer 1**: Supports both one-shot and free-running modes (selected by ACR
bit 6). In free-running mode, the counter reloads from the latch on each
underflow and the interrupt fires repeatedly. In one-shot mode, the interrupt
fires once after the first underflow following a T1CH write.

**Timer 2**: Always one-shot. Counts CPU clock cycles when ACR bit 5 is 0.
The counter is loaded by writing T2CL (low byte latched) then T2CH (loads
counter and arms the timer). Reading T2CL clears the T2 interrupt flag.

Both timers are decremented by `via_tick(cycles)` called after each CPU
execution slice (~1000 cycles). Underflow is detected by checking if the
counter value wrapped above the pre-decrement value (unsigned overflow).

### Overlay Mechanism

The overlay is implemented as a one-shot latch:

```
Power On
   |
   v
overlay = true   (ROM at address 0)
   |
   v
ROM copies vectors from ROM to RAM[0x000000]
   |
   v
First write to VIA ORA (any value)
   |
   v
overlay = false  (RAM at address 0, PERMANENTLY)
overlay_was_off = true
   |
   v
All subsequent ORA writes: overlay stays false
```

The `overlay_was_off` flag enables a special case in the memory decoder: if
overlay is ever re-enabled (which does not happen with the one-shot
implementation, but is guarded against), vector table reads
(`0x0000-0x03FF`) come from RAM, not ROM, preserving the installed exception
handlers.

---

## 7. Video Pipeline

### Mac Classic Display Format

The original Macintosh Classic has a built-in 9-inch monochrome CRT:
- Resolution: 512 x 342 pixels
- Bit depth: 1 bit per pixel (monochrome)
- Pixel encoding: **1 = black, 0 = white** (inverted from typical conventions)
- Stride: 64 bytes per scanline (512 / 8)
- Total framebuffer: 21,888 bytes (64 * 342)
- Location: top of RAM at `RAM_SIZE - 0x5900`
- Bit order within bytes: bit 7 = leftmost pixel, bit 0 = rightmost

### Conversion Pipeline

```
Mac RAM Framebuffer          Scaling              Display
512 x 342, 1-bit     -->   Nearest-     -->   480 x 320
(21,888 bytes)              Neighbor           RGB565
                            Lookup             (307,200 bytes
                            Tables              if full frame)

                            But rendered in
                            20-line chunks
                            (19,200 bytes each)
```

### Nearest-Neighbor Scaling

Two precomputed lookup tables map display coordinates to source coordinates:

```c
x_src_table[480]  // For each display X (0-479), the source Mac X (0-511)
y_src_table[320]  // For each display Y (0-319), the source Mac Y (0-341)
```

The scaling ratios are:
- X: 480/512 = 0.9375 (slight horizontal shrink, loses ~6% of pixels)
- Y: 320/342 = 0.9357 (slight vertical shrink, similar ratio)

The near-identical X and Y ratios mean the display is approximately
aspect-correct.

### Chunked DMA Rendering

Rendering the entire 480x320 frame in RGB565 would require a 307,200-byte
buffer, far exceeding available DMA-capable SRAM. Instead, the frame is
rendered in **20-line strips** (chunks):

```
Display (480 x 320)
+================================+  chunk 0: lines 0-19
|  20 lines = 480*20*2 = 19,200 |  (19,200 bytes)
+================================+  chunk 1: lines 20-39
|  20 lines                      |
+================================+
|  ...                           |
+================================+  chunk 15: lines 300-319
|  20 lines                      |
+================================+
```

Each chunk:
1. Converts 20 lines from 1-bit to RGB565 into a single DMA buffer
2. Calls `display_draw()` which issues `esp_lcd_panel_draw_bitmap()`
3. Blocks on a semaphore until the SPI DMA transfer completes
4. Reuses the same buffer for the next chunk

The DMA buffer is allocated in internal SRAM (`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`) because the ESP32's SPI DMA controller cannot access PSRAM.

**Buffer size**: 480 * 20 * 2 = 19,200 bytes per chunk.

### Per-Pixel Conversion

For each pixel in the output chunk:

```c
int sx = x_src_table[dx];               // Lookup source X
int byte_idx = sx >> 3;                  // Byte in source scanline
int bit_idx  = 7 - (sx & 7);            // Bit within byte (MSB = left)
int pixel = (src_line[byte_idx] >> bit_idx) & 1;
*out++ = pixel ? 0x0000 : 0xFFFF;       // 1=black, 0=white
```

### Frame Rate

The emulator targets 60 fps (matching Mac VBlank rate). After executing one
frame's worth of CPU cycles (~130,560) and rendering the display, the main
loop calculates remaining time in the 16,667 us frame period and sleeps via
`vTaskDelay`. If the frame took longer than the period, it yields to other
FreeRTOS tasks with `taskYIELD()`.

---

## 8. SCC Z8530 Emulation

The Zilog 8530 Serial Communications Controller provides two independent
serial channels. On the Macintosh Classic:
- **Channel A**: Modem port (active during boot for diagnostic handshake)
- **Channel B**: Printer port

### Address Decoding

| Address Range | Bit 2 | Bit 1 | Operation |
|---------------|-------|-------|-----------|
| `0x800000-0x9FFFFF` | 0/1 | 0 | Channel B control read |
| `0x800000-0x9FFFFF` | 0/1 | 1 | Channel A control read |
| `0xA00000-0xBFFFFF` | 0/1 | 0 | Channel B control write |
| `0xA00000-0xBFFFFF` | 0/1 | 1 | Channel A control write |

Bit 2 selects control (0) vs data (1). Bit 1 selects channel B (0) vs A (1).

### Register Pointer Mechanism

The Z8530 uses an indirect addressing scheme. To access register N:
1. Write N to WR0 (bits 2-0 = register number, bits 5-3 = command)
2. The next read or write accesses RR_N or WR_N
3. The pointer automatically resets to 0

For registers 8-15, the "point high" command (cmd=1 in WR0 bits 5-3) adds 8
to the register number.

### Default Register Values

```
RR0 = 0x6C:  Tx Buffer Empty (bit 2) = 1
             DCD (bit 3) = 1 (inactive, no carrier)
             Tx Underrun (bit 6) = 1
             CTS (bit 5) = 1 (clear to send, inactive)
             Rx Available (bit 0) = 0 (no data)

RR1 = 0x07:  All Sent (bit 0) = 1
             Residue code bits set

RR3 = 0x00:  No interrupts pending
```

The DCD=1 (inactive) default is critical. If DCD were 0 (active), the ROM
would think a modem carrier is detected and poll for serial data indefinitely.

### Boot Handshake (Not Injected)

During boot, the ROM sends `*APPLE*` on channel A and listens for a `*S`
response. This is the Mac's serial diagnostic handshake -- if a debugger
responds with `*S`, the ROM enters service mode.

The emulator does **not** inject the `*S` response. Doing so was attempted
and found to be counterproductive: the `*S` handshake enters service mode,
which is another infinite loop waiting for debugger commands. Instead, the
dispatch loop escape bypass (Section 5.4) handles this situation by redirecting
PC to the ROM's register-restore-and-RTS sequence after a few frames.

---

## 9. IWM/SWIM Floppy Emulation

### IWM Register Model

The IWM (Integrated Woz Machine) uses a state-machine-based register select
rather than a conventional address-mapped register file. Address lines A12-A9
(mapped from the bus via the motherboard PAL) control four state lines:

| A12 (reg bit 3) | A11 (reg bit 2) | A10 (reg bit 1) | A9 (reg bit 0) |
|------------------|------------------|-------------------|-----------------|
| LSTRB | CA2 | CA1 | CA0 |

Even-numbered registers **clear** the corresponding state line; odd-numbered
registers **set** it. Additional state lines (ENABLE, SELECT, Q6, Q7) are
similarly controlled by higher register indices.

### Read Mode Selection

The Q6 and Q7 state lines determine what is returned on a read:

| Q7 | Q6 | Read Returns |
|----|----|----|
| 0 | 0 | Data register / disk status (sense lines based on CA0-CA2) |
| 0 | 1 | Status register (mode bits + motor state) |
| 1 | 0 | Handshake register (0x3F = ready) |
| 1 | 1 | (Write mode register -- not a read) |

### Sense Line Responses

When Q6=0, Q7=0, the CA0-CA2 bits select which sense line to report:

| CA2:CA0 | Sense | Value |
|---------|-------|-------|
| 0 | /STEP direction | 0x00 |
| 1 | Disk in place | 0xFF (no disk) |
| 2 | /STEP (not at track 0) | depends on track |
| 3 | /WRPROT | 0xFF (write protected) |
| 4 | Motor running | 0x00 |
| 6 | TACH | 0x00 |

### Current Limitation

The IWM stub can report sense lines but **cannot emulate sector-level disk
reads**. The ROM's disk driver issues read commands and then busy-waits for
a completion interrupt (BTST #5 on a RAM flag set by the interrupt handler).
Since the stub never completes reads, the IWM busy-flag bypass (Section 5.5)
clears the flag manually so the ROM can discover "no valid boot blocks" and
display the "?" floppy icon.

A disk image (1.44 MB `OS_750_boot.dsk`) is embedded in flash and copied to
PSRAM at boot for future use, but is currently **disabled** in `iwm_init()`:
```c
iwm.disk_inserted = false;
iwm.disk_data = NULL;
```

---

## 10. Challenges Solved

This section documents the major debugging journey in chronological order.
Each problem was discovered through serial monitor logs and targeted
instrumentation of the emulation loop.

### 10.1 Inverted Overlay Logic -- Memory Test Failures

**Symptom**: The ROM's memory test at `0x401CF0` always found D6 != 0 (errors
detected), causing the boot to fail or enter the dispatch loop.

**Root cause**: The overlay bit was tracked as bit 4 of `VIA.ORA`. Every write
to ORA -- even those intended to control sound volume or screen buffer -- would
toggle the overlay based on bit 4's value. The ROM writes ORA many times after
initial boot for non-overlay purposes. Each write could flip the address map,
causing the memory test to read ROM where it expected RAM.

**Fix**: Implemented the one-shot latch behavior: overlay clears on the first
ORA write and stays cleared permanently. This matches the real Mac hardware
where a PAL-based flip-flop controls overlay.

**Evidence**: After the fix, D6 is consistently 0x00000000 at all three
checkpoint PCs, and the D6 bypass no longer fires.

### 10.2 Discovery of the One-Shot Overlay Latch

**Symptom**: Even after fixing overlay to not toggle on every write, the
emulator originally tried to track bit 4 of ORA: overlay ON when bit 4 is set,
OFF when clear. This caused a subtler bug where overlay would re-enable during
later ROM initialization.

**Discovery**: Analysis of the Mac Classic hardware schematics and the
"Guide to the Macintosh Family Hardware" revealed that the overlay is not
controlled by ORA bit 4 at all -- it is a **one-shot latch** that is SET on
reset and CLEARED by the first write to VIA address space. The actual value
written is irrelevant.

**Impact**: This was the single most important fix for boot reliability. All
subsequent overlay-related issues disappeared.

### 10.3 Dispatch Loop Analysis -- No Exit Path

**Symptom**: After passing memory tests, the ROM entered `0x401F38` (the serial
debug dispatch loop) and never left. The CPU spun reading SCC registers
indefinitely.

**Analysis**: Disassembly of the ROM revealed:
1. The dispatch loop is the Mac's built-in serial debug monitor
2. It is entered via the service dispatch at `0x401DB0`
3. The service dispatch saves all registers with MOVEM (62 bytes on stack)
4. Every possible code path through the dispatch loop ends with `BRA.W` back
   to `0x401F4A` -- there is no conditional exit
5. Even receiving the `*S` handshake enters "service mode" -- another loop

**Fix**: The bypass redirects PC to `0x401F2C`, the ROM's own cleanup sequence
that restores all MOVEM-saved registers from the stack and executes RTS. This
is a clean exit because:
- The stack frame is exactly what the service dispatch pushed
- All registers (D0-D7, A0-A6, SR) are properly restored
- RTS returns to the caller (the init subroutine that entered dispatch)

**Why 5 frames?** The ROM enters the dispatch loop and executes many iterations
per frame (each iteration is just SCC register reads + branch). Waiting 5
frames ensures the ROM has fully settled into the loop and is not in a
transient state.

### 10.4 IWM Disk Driver Busy-Wait

**Symptom**: After escaping the dispatch loop, the ROM proceeded to disk
driver initialization but then froze at `0x40335E`.

**Analysis**: The ROM's disk driver at `0x40335E` executes:
```
BTST  #5, ($015D, A3)   ; Test bit 5 of disk driver flag byte
BNE.S $40335E           ; Loop if still set (busy)
```
Register A3 points to the disk driver's private data area in RAM. Bit 5 is a
"busy" flag that the IWM interrupt handler clears when a sector read completes.
Since the IWM stub cannot generate completion interrupts, the flag stays set
forever.

**Fix**: After 3 consecutive frames with PC in the busy-wait range, the bypass
reads A3, calculates the flag address (`A3 + 0x015D`), and clears bit 5 in
RAM. The ROM then checks the read result, finds no valid boot data, and
continues to the "?" floppy icon display.

### 10.5 SCSI Stub -- 0xFF vs 0x00

**Symptom**: During debugging of later boot stages, the ROM occasionally
entered error handlers when probing SCSI devices.

**Root cause**: The SCSI address range (`0x580000-0x5FFFFF`) was returning
`0xFF` (the default for unmapped space). On the NCR 5380 SCSI controller,
`0xFF` means all bus signals are asserted -- BSY, SEL, REQ, I/O, C/D, MSG all
active. The ROM interprets this as a stuck bus.

**Fix**: Return `0x00` for the entire SCSI range, indicating "bus free, no
devices." The ROM's SCSI probe sees no devices and moves on cleanly.

### Debugging Timeline Summary

```
 Phase 1: Black screen, immediate crash
    --> Overlay was toggling on every ORA write
    --> Fix: One-shot overlay latch

 Phase 2: Memory test fails, D6 != 0
    --> Overlay bit-4 tracking still wrong
    --> Fix: Overlay ignores bit value, clears on first write

 Phase 3: Boot reaches dispatch loop, hangs
    --> Serial debug monitor has no exit
    --> Fix: PC redirect to ROM's own MOVEM/RTS cleanup

 Phase 4: Dispatch escaped, stuck in IWM busy-wait
    --> IWM stub can't complete sector reads
    --> Fix: Clear busy flag in RAM after timeout

 Phase 5: SCSI probe confusion
    --> 0xFF = all bus lines asserted
    --> Fix: Return 0x00 for SCSI range

 Phase 6: Boot reaches "?" floppy icon -- SUCCESS
    --> ROM fully booted, display working
```

---

## 11. PSRAM Budget

The ESP32-WROVER-B has 8 MB of physical PSRAM, but the ESP32's SPI cache
window can only map ~4 MB into the CPU's virtual address space at once.

### Allocation Table

| Allocation | Size | Type | Source |
|------------|------|------|--------|
| Musashi opcode jump table | ~320 KB | BSS | `EXT_RAM_BSS_ATTR` (static) |
| Musashi cycle count table | ~260 KB | BSS | `EXT_RAM_BSS_ATTR` (static) |
| Mac RAM | 2,048 KB (2 MB) | Heap | `heap_caps_calloc(MALLOC_CAP_SPIRAM)` |
| Disk image (writable copy) | 1,440 KB (1.44 MB) | Heap | `heap_caps_malloc(MALLOC_CAP_SPIRAM)` |
| **Total** | **~4,068 KB** | | |
| **Headroom** | **~28-100 KB** | | |

### Why 2 MB Mac RAM Instead of 4 MB?

The original Mac Classic shipped with 1 MB of RAM, expandable to 4 MB. The
emulator uses 2 MB as a compromise:
- 4 MB Mac RAM + 1.44 MB disk + 580 KB Musashi = ~6 MB, exceeding the ~4 MB
  usable PSRAM
- 2 MB is double the original Mac's memory, sufficient for System 7.5
- This leaves enough room for the disk image (needed for future boot support)

### Why Not Use Internal SRAM for Mac RAM?

Internal SRAM is only ~365 KB free, of which ~302 KB is DMA-capable. The
Mac RAM (2 MB minimum for useful operation) can only come from PSRAM. Internal
SRAM is reserved for:
- FreeRTOS stacks and kernel objects
- DMA buffers (video chunk buffer: 19,200 bytes)
- ESP-IDF system allocations

### Why Copy Disk Image to PSRAM?

The disk image is embedded in firmware flash via `EMBED_FILES` in CMakeLists.txt
and accessible as a const array in `.rodata`. However, future disk write support
(OS installation) requires writable storage. The image is copied from flash to
PSRAM at boot so it can be modified at runtime. Currently the copy is made but
the IWM stub does not use it (disk_inserted is forced false).

---

## 12. Build System

### ESP-IDF v6.0

The project uses ESP-IDF v6.0 installed via Espressif's EIM (ESP-IDF Manager)
at `C:\esp\v6.0\esp-idf` with toolchain at `C:\Espressif\tools`.

### CRITICAL: cmd.exe Requirement

ESP-IDF v6.0 on Windows **rejects MSYS/MinGW shell environments**. The
`idf.py` script detects MSYS/MinGW environment variables and refuses to run.
All builds MUST go through `cmd.exe`:

```bash
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat build"
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat flash"
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat monitor"
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat size"
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat fullclean"
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat set-target esp32"
```

The `build.bat` wrapper:
1. Sets `IDF_PATH`, `IDF_TOOLS_PATH`, and the full toolchain `PATH`
2. Clears `MSYSTEM`, `MSYS`, `MINGW_PREFIX`, `MINGW_CHOST` environment
   variables to prevent ESP-IDF's shell detection from triggering
3. Changes to the project directory
4. Invokes `idf.py` with the provided arguments

After a `fullclean`, you must run `set-target esp32` before building.

### Project CMake Structure

```
MacintoshESPEmulator/
  CMakeLists.txt              # Top-level: sets EXTRA_COMPONENT_DIRS, includes idf.py cmake
  main/
    CMakeLists.txt            # Registers main sources, embeds ROM/disk, sets HAS_DISK_IMAGE
  components/
    musashi/
      CMakeLists.txt          # Registers m68kcpu.c + m68kops.c, suppresses warnings
    display/
      CMakeLists.txt          # Registers display.cpp, depends on esp_lcd + drivers
```

### ROM and Disk Image Embedding

The ROM and optional disk image are embedded directly in the firmware binary
via `EMBED_FILES` in `main/CMakeLists.txt`:

```cmake
set(EMBED_FILES "${ROM_DIR}/Classic.ROM")
if(EXISTS "${ROM_DIR}/OS_750_boot.dsk")
    list(APPEND EMBED_FILES "${ROM_DIR}/OS_750_boot.dsk")
    set(HAS_DISK_IMAGE 1)
endif()
```

The linker generates symbols `_binary_Classic_ROM_start` / `_binary_Classic_ROM_end`
and `_binary_OS_750_boot_dsk_start` / `_binary_OS_750_boot_dsk_end` that the C
code references via `asm()` attribute declarations.

### Partition Table

The 4 MB flash is divided into a single large application partition:

| Name | Type | Offset | Size |
|------|------|--------|------|
| nvs | data (nvs) | 0x9000 | 24 KB |
| phy_init | data (phy) | 0xF000 | 4 KB |
| factory | app | 0x10000 | ~3,968 KB |

The factory partition must hold the firmware code + embedded ROM (512 KB) +
embedded disk image (1,440 KB), totaling roughly 2.4 MB of embedded data plus
the compiled code.

### Musashi Code Generation

Musashi requires a two-stage build:
1. Compile `m68kmake.c` with a host C compiler (e.g., MinGW GCC)
2. Run `m68kmake . m68k_in.c` to generate `m68kops.c` and `m68kops.h`

The generated files are committed to the repository and do not need
regeneration unless `m68k_in.c` is modified.

The Musashi component suppresses numerous GCC warnings because the code
predates modern compiler strictness (GCC 15.2.0 in the ESP-IDF v6.0
toolchain is particularly aggressive about `-Wshift-count-overflow`,
`-Woverflow`, `-Warray-bounds`, etc.).

### SDK Configuration Highlights

From `sdkconfig.defaults`:
- CPU frequency: 240 MHz (maximum)
- Flash: 4 MB, 80 MHz DIO mode
- PSRAM: enabled, quad SPI, 80 MHz
- PSRAM allocation: `CAPS_ALLOC` mode (explicit via `heap_caps_malloc`)
- PSRAM BSS: enabled (`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`)
  for Musashi's `EXT_RAM_BSS_ATTR` tables
- FreeRTOS tick rate: 1000 Hz
- Compiler optimization: performance (`-O2`)

---

## 13. Current Status and Future Work

### Current Status (as of 2026-04-09)

The emulator successfully boots the Mac Classic ROM to the point where:
- ROM enters, initializes hardware
- Memory tests pass (with overlay one-shot latch correctly implemented)
- Serial debug dispatch loop is escaped via PC redirect
- IWM disk driver busy-wait is resolved via flag clearing
- SCSI probe completes without error
- The "?" floppy icon is displayed on the physical LCD
- Touch input is mapped to Mac mouse coordinates
- VBlank interrupts fire at ~60 Hz
- VIA timers (T1 and T2) operate correctly
- Frame rate is approximately 60 fps with chunked video rendering

### Known SCC Polling Bottleneck

The ROM's SCC initialization polls channel A RR0 extensively (tens of thousands
of reads during dispatch). The current RR0 default of `0x6C` includes DCD=1 and
CTS=1 (inactive). Changing RR0 to `0x44` (clearing DCD/CTS to indicate no
serial devices at all) may reduce the time the ROM spends in SCC polling before
the dispatch loop escape fires. This is a planned optimization.

### Future Work

**Short-term**:
- **SCC RR0 optimization**: Change default from `0x6C` to `0x44` to reduce
  serial polling during dispatch
- **Keyboard input**: Implement touch-screen virtual keyboard, encoding
  keypresses via VIA shift register (SR) and CB1/CB2 signals
- **RTC emulation**: Currently the RTC data line (PB0) idles high. Implement
  basic RTC responses for date/time (the ROM reads RTC during boot)

**Medium-term**:
- **IWM sector-level reads**: Implement proper GCR/MFM sector read emulation
  so the ROM can read boot blocks from the embedded disk image. This would
  allow booting a System 7 floppy.
- **Virtual SCSI hard drive**: Emulate a SCSI hard drive using a PSRAM buffer
  or flash partition. Requires NCR 5380 register emulation beyond the current
  bus-free stub. Would enable installing and running Mac OS from a virtual
  hard drive.

**Long-term**:
- **68030 upgrade**: Musashi supports 68030 emulation. The cycle tables already
  carry data for all CPU types (`NUM_CPU_TYPES=5`). To enable:
  1. Change `M68K_EMULATE_030` to `OPT_ON` in `m68kconf.h`
  2. Set CPU type to `M68K_CPU_TYPE_68030` in `mac_emulator.c`
  3. Use a 68030-era ROM (Color Classic or LC II ROMs in `ROM/`)
  This would enable running later Mac OS versions with 32-bit addressing.
- **Sound output**: The Mac's sound buffer is at `RAM_SIZE - 0x0300`. An I2S
  or PWM DAC could output the 8-bit audio samples.
- **Serial port passthrough**: Map the SCC to the ESP32's UART for real serial
  communication (e.g., terminal access to Mac applications).
