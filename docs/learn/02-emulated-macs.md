# 2. The emulated Macs

One codebase emulates two different Macintoshes. The build picks one with
`ROM_NAME` in `main/CMakeLists.txt`:

| | Macintosh Classic (1990) | Macintosh LC II (1992) |
|---|---|---|
| ROM | `Classic.ROM`, 512 KB | `LC-II.ROM`, 512 KB |
| CPU | 68000 at 7.83 MHz | 68030 (real: 16 MHz; here, see chapter 3) |
| Chipset | Discrete chips: VIA, IWM/SWIM, SCC, SCSI, RTC | **V8** system chip + VIA1, SWIM, SCC, SCSI, Egret |
| Video | 1-bit, 512×342 framebuffer inside main RAM | Colour, 1–8 bpp (16 bpp also handled), separate VRAM and a CLUT |
| Keyboard/mouse/clock | ADB via the VIA shift register; RTC chip on the VIA | **Egret** microcontroller (ADB, clock, PRAM) |
| Compile switch | `MAC_ROM_LC_II=0` | `MAC_ROM_LC_II=1`, `MAC_V8_CUDA=0` |

A third ROM, the Color Classic (`Color-Classic.ROM`, 1 MB, Cuda instead of
Egret), uses the same V8 code. It is kept only as a reference, because it
costs 512 KB more flash and runs no faster.

Reference: addresses in [`docs/memory-map.md`](../memory-map.md), device
details in [`docs/peripherals.md`](../peripherals.md). This chapter explains
how the pieces fit together.

## What a 68k Mac is made of

Every 68k Mac follows the same pattern. A CPU sees one flat address space
(24 bits here: 16 MB). Address ranges are decoded to RAM, ROM, or a chip.
The chips raise interrupts on a few priority levels. There is no I/O
instruction: talking to a chip means reading or writing its addresses.

So an emulator has three jobs:

1. **Run the CPU** (Musashi, [chapter 3](03-cpu-emulation.md)).
2. **Decode addresses**: every CPU read or write calls a C function that
   decides what lives at that address (`main/mac_memory.c`).
3. **Model the chips** well enough that the ROM and the System believe them.

## The Mac Classic

The Classic is a "compact Mac", a direct descendant of the 1984 original.
The address decoding is simple and done by a few PALs: the top address bits
select the device.

```
 Mac Classic, 24-bit bus (normal mode)
 $000000 ┌────────────────────────────┐
         │ RAM 2 MB (mirrored up to   │  low-memory globals, trap tables,
         │ $3FFFFF: ram_mask $1FFFFF) │  heaps, and the 1-bit framebuffer
 $400000 ├────────────────────────────┤  at MemTop − $5900 = $1FA700
         │ ROM 512 KB (mirrored)      │
 $580000 ├────────────────────────────┤
         │ SCSI NCR 5380              │  empty bus (no disk attached)
 $800000 ├────────────────────────────┤
         │ SCC read                   │  serial stub
 $A00000 ├────────────────────────────┤
         │ SCC write                  │
 $C00000 ├────────────────────────────┤
         │ IWM / SWIM floppy          │  odd bytes; A1–A4 select a line
 $E00000 ├────────────────────────────┤
         │ VIA 6522 (at $EFE1FE)      │  timers, VBL, one-second IRQ,
         │                            │  RTC, mouse button, ADB
 $F00000 └────────────────────────────┘
```

The two key chips:

- **VIA 6522** (`mac_via.c`). The Mac's general-purpose chip: two timers (the
  OS calibrates its delay loops and runs the Time Manager on them), the VBL
  interrupt (60.15 Hz) on CA1, a one-second interrupt on CA2, a shift
  register for ADB, and two 8-bit ports with assorted control lines (ROM
  overlay, sound volume, RTC data/clock, mouse button). All of it reaches the
  CPU as interrupt **level 1**.
- **SWIM** (`mac_iwm.c`). The floppy controller. It has two personalities:
  the old IWM (a few state lines, GCR disks) and the newer ISM (registers,
  MFM 1.44 MB disks). The emulator builds the real MFM byte stream of each
  track (gaps, sync marks, CRCs) so the ROM's own driver can read it.

The framebuffer is just Mac RAM: the video hardware reads 21,888 bytes
(512 × 342 / 8) near the top of RAM. To "display" it, the renderer reads the
same bytes ([chapter 5](05-input-display-storage.md)).

## The LC II and the V8 chip

The LC II belongs to a later generation. Apple's **V8** chip integrates the
memory controller, the RAM-based video (VRAM, colour lookup table) and a
"pseudo-VIA2". A 6805 microcontroller called **Egret** takes over ADB, the
real-time clock and PRAM.

The device addresses were not guessed. They come from **the ROM's own
decoder table** (a table of base addresses the ROM uses for each machine; in
the Color Classic ROM at file offset $3490, same constants in the LC II).

```
 LC II (V8), 24-bit view
 $000000 ┌────────────────────────────┐
         │ RAM (2 MB populated;       │  above 2 MB reads $FF, no mirror,
         │ space up to $9FFFFF)       │  so the ROM sizes RAM correctly
 $A00000 ├────────────────────────────┤
         │ ROM 512 KB                 │  (32-bit address $40A00000)
 $B00000 ├────────────────────────────┤
         │ (unused)                   │
 $F00000 ├────────────────────────────┤
         │ VIA1          $F00000      │  timers, VBL, Egret handshake
         │ SCC           $F04000      │  stub
         │ SCSI 5380     $F10000      │  empty bus
         │ ASC (sound)   $F14000      │  not emulated (reads $FF)
         │ SWIM          $F16000      │
         │ VDAC / CLUT   $F24000      │  palette
         │ pseudo-VIA2   $F26000      │  slot VBL (IRQ level 2), monitor
         │                            │  sense, depth
         │ VRAM 512 KB   $F40000      │  in PSRAM, ScrnBase $50F40000
         │ diag ROM probe $FC0000     │  reads $FF
         │ .Sony stub    $FE0000      │  emulator-only (chapter 5)
 $FFFFFF └────────────────────────────┘
```

The LC II ROM runs the 68030 in 24-bit mode. The 32-bit device bases
(`$50Fxxxxx`) reduce to `$Fxxxxx` after masking to 24 bits, so one 24-bit
decoder serves both views. The emulated LC II has **2 MB of RAM** (the real
machine has 4 MB on the board); 2 MB fits the PSRAM budget.

## How a Mac ROM boots: the overlay

A 68000 starts by reading two long words from address 0: the initial stack
pointer and the initial program counter. On a Mac, address 0 should be RAM
(it holds the exception vectors and the OS globals), but RAM is empty at
power-on. The trick is the **overlay**: right after reset, the ROM also
appears at address 0. The ROM then turns the overlay off.

The first eight bytes of both ROMs, as the CPU reads them:

| ROM | Long 0 (→ SSP) | Long 1 (→ PC) |
|---|---|---|
| Classic | `$A49F9914` | `$0040002A` (absolute ROM address) |
| LC II | `$35C28F5F` | `$0000002A` (inside the overlay mirror at 0) |

The first long is really the ROM **checksum**; the CPU loads it as a stack
pointer, and the ROM sets a real one soon after. At offset `$2A` both ROMs
start with `4E FA 00 60`: `JMP $8C(PC)`. (Misreading these four bytes cost
two debugging attempts; see [chapter 6](06-lessons-learned.md).)

The two machines turn the overlay off differently, and getting this wrong
broke the V8 bring-up:

| | Classic | LC II (V8) |
|---|---|---|
| While the overlay is on | ROM at $000000; RAM moved to $600000 | ROM mirrored at $000000–$07FFFF only; RAM never moves |
| Overlay clears when | the first write to VIA port A (ORA) | the first access to the real ROM window $A00000 (the LC II ROM jumps there at $A02E3E) |
| Emulation | one-shot latch: later ORA writes (sound, screen select) must not bring it back | `via_clear_overlay()` called from the memory decoder, as in MAME |

After the overlay, the boot follows the usual Mac path: hardware tests, RAM
sizing, video setup, look for a boot disk, read its boot blocks, load the
System file, run INITs (extensions), start the Process Manager, launch the
Finder.

## What is emulated, and how deeply

Not every chip needs a faithful model. The emulator uses three levels:

| Level | Meaning | Examples |
|---|---|---|
| **Register-level** | The chip's registers behave like the real ones; the ROM's own driver runs unchanged | VIA timers, IFR/IER and ports; SWIM ISM/IWM and drive sense lines; NCR 5380 registers (no devices); Classic RTC serial protocol; VDAC/CLUT; pseudo-VIA2 |
| **High-level (HLE)** | We imitate what the OS sees, not how the chip works inside | Egret/Cuda (the packet protocol, not the 6805 program); .Sony disk reads (served straight from the image); the PMMU (instructions accepted, no translation); the Classic's ADB (bypassed); mouse position (written into OS globals) |
| **Stub / absent** | Just enough that the OS does not hang | SCC (status bits only), ASC/sound, FPU (compiled in, unused) |

Why HLE for Egret? The real Egret runs its own 6805 firmware, which we do not
have and do not want to emulate cycle by cycle. The ROM talks to it through
a byte handshake on VIA1 port B and the VIA shift register. `mac_egret.c`
answers that handshake and the packets the ROM sends: PRAM reads and writes,
the clock, IIC, ADB autopoll and a mouse. One subtle job: the Egret, not the
VIA, keeps the Mac's clock ticking. It sends an unsolicited packet every
second, and the ROM adds 1 to `Time`. Until that packet was emulated, the
LC II's clock was frozen.

Why HLE for the PMMU? The LC II ROM sets up 68030 MMU tables, but those
tables map addresses exactly the same way as plain 24-bit masking. That was
checked. Translating every memory access would only cost time. So
`m68kmmu.h` accepts the MMU instructions (PMOVE, PLOAD, PFLUSH, PTEST) with
the correct operand lengths and changes nothing.

Why HLE for disks? Reading through the emulated SWIM works (the Classic
build still has that path), but it costs many emulated instructions per
byte. Serving whole requests from the image at the driver's entry point is
far cheaper. [Chapter 5](05-input-display-storage.md) explains the hooks.

### Classic boot workarounds

The Classic build carries a list of targeted workarounds from its bring-up:
forcing the memory test result, skipping SCSI polling, a one-shot overlay
latch, ADB completion flags, a StartUpScreen branch patch, and more. Each is
keyed to a Classic ROM address and gated off on the LC II. The list and the
reason for each are in
[`docs/architecture-overview.md`](../architecture-overview.md#classic-boot-workarounds-history).
The LC II needed far fewer, because its bring-up used the host harness and
an instruction trace from the start.

## Making the Mac use a 480×320 screen

The LCD is 480×320. The Classic's screen is 512×342 and the LC II's
built-in monitor mode is 512×384. Scaling loses detail: 512→480 drops one
pixel column in 16, and 384→320 drops one line in six. Thin lines and text
suffer.

The cleaner solution: **make the Mac believe its screen is 480×320**, then
copy pixels 1:1. Both machines do this with ROM patches applied on read
(the image in flash stays intact):

| | Classic (`CLASSIC_NATIVE_LCD`) | LC II (`V8_NATIVE_LCD`) |
|---|---|---|
| Where the size lives | Immediate operands in a few ROM routines | Video parameter blocks (VPBlocks) in the declaration ROM, read by the Slot Manager |
| Patch | 5 immediates: cursor bounds (InitCrsr), `ScrnSize`, `_ScrnBitMap` → 480/320 | 13 VPBlocks for 512×384 → 480×320, then the Slot Manager CRC is recomputed |
| Framebuffer layout | Unchanged (rowBytes 64, 512×342); the extra columns and rows are an unused margin | rowBytes 1024 in VRAM; bounds 480×320 |
| Precedent | Mini vMac's "screen hack" | Same idea, applied to the slot declaration data |

On the LC II the patched copy of the declaration ROM (~5 KB in PSRAM) is
served for reads in that area. The ROM's own startup checksum still reads
the original bytes and passes.

Some programs assume 512×342 anyway. Full-screen games are the main case:
the Classic build detects them and switches to a scaled "full view" (see
[chapter 5](05-input-display-storage.md)).

## The two machines compared

| Topic | Classic | LC II |
|---|---|---|
| Interrupts | VIA → level 1 | VIA1 → level 1, pseudo-VIA2 slot VBL → level 2 |
| Clock / PRAM | RTC chip on VIA port B (`mac_rtc.c`) | Egret HLE |
| Mouse button | Event posted by a C mirror of the ROM's mouse handler | Emulated ADB mouse through Egret; the ROM posts the event |
| Floppy | SWIM ISM stream + .Sony Device Manager intercepts | SWIM for detection + .Sony Prime hook |
| Boot to Finder, board | ~63 s (with native speed-ups, custom disk) | ~115 s (measured before the native speed-ups) |
| Why choose it | Fastest; the 68000 core has the least decode work and the screen is 1-bit | Colour on demand, 68030, newer software |

Speed note: the ROM does not make the emulator faster or slower. System 7.5.3
code dominates either way. What differs is the CPU core's cost per
instruction and the screen depth. This is discussed in
[chapter 4](04-performance.md).

Next: [3. CPU emulation with Musashi](03-cpu-emulation.md).
