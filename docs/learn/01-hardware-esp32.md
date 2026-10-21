# 1. The ESP32 board as a computer

Before looking at the emulated Mac, look at the machine that runs it. Most
decisions in this project come from the shape of the ESP32's memory system,
not from the Mac.

Reference: pins, datasheets and measured specs are in
[`docs/hardware.md`](../hardware.md). This chapter explains how the parts
work together.

## The board

The **WT32-SC01 v3.4** (Wireless Tag) is an ESP32-WROVER-B module soldered
behind a 3.5" LCD with a capacitive touch panel. The emulator needs no other
hardware.

```
                      WT32-SC01 v3.4
 ┌──────────────────────────────────────────────────────────────────┐
 │  ESP32-WROVER-B module                                           │
 │ ┌──────────────────────────────────────────────┐                 │
 │ │ ESP32-D0WD rev 1.0                           │   QIO 80 MHz    │
 │ │  ┌────────┐  ┌────────┐                      ├──── SPI ────────┼─► Flash 4 MB
 │ │  │ core 0 │  │ core 1 │   240 MHz Xtensa LX6 │   (shared bus)  │
 │ │  └───┬────┘  └───┬────┘                      ├──── SPI ────────┼─► PSRAM 8 MB
 │ │  ┌───┴──┐    ┌───┴──┐   32 KB cache per core │   quad 80 MHz   │   (4 MB mapped)
 │ │  │cache │    │cache │   (flash + PSRAM)      │                 │
 │ │  └──────┘    └──────┘                        │                 │
 │ │  internal SRAM 520 KB (IRAM + DRAM)          │                 │
 │ │  SPI2 ──────────────────────────────── 40 MHz├─────────────────┼─► ST7796S LCD 480×320
 │ │  I2C0 ──────────────────────────────── 400kHz├─────────────────┼─► FT5x06 touch (0x38)
 │ │  UART0 ────────────────────────────────────  ├─────────────────┼─► CH340 USB serial
 │ └──────────────────────────────────────────────┘                 │
 └──────────────────────────────────────────────────────────────────┘
```

What the board diagnostic (`board_test/`) measured:

| Part | Label / datasheet | Measured |
|---|---|---|
| SoC | ESP32-WROVER-B | ESP32-D0WD **rev 1.0**, 2 × Xtensa LX6 at 240 MHz |
| Flash | 16 MB | **4 MB**, run at 80 MHz QIO |
| PSRAM | 8 MB | 8 MB, but only **~4 MB addressable** |
| Internal SRAM | 520 KB | ~365 KB free at boot, ~302 KB of it DMA-capable |

Two of these numbers set hard limits: **4 MB of flash** holds the code, the
Apple ROM *and* the whole disk image, and **4 MB of mappable PSRAM** holds
the Mac's RAM and everything else that is large.

## Two cores, three jobs

The ESP32 has two identical cores. The emulator gives the whole of core 1 to
the 68k CPU and puts everything else on core 0.

```
 core 1                                   core 0
 ──────                                   ──────
 mac_emu task (prio MAX-2, 32 KB stack)   mac_video task (prio MAX-3, 8 KB stack)
   endless frame loop                       render Mac screen → LCD by SPI DMA
   Musashi + device models                touch task (prio MAX-2, 3 KB stack)
   idle fast-forward, throttle              read FT5x06 over I2C every 16 ms
                                          (FreeRTOS housekeeping, idle task)
```

Task creation is in `main/main.cpp` and `touch_sampler_start()` in
`main/mac_touch.c`.

Why split it this way:

- **The CPU interpreter is the bottleneck.** On the first profiles, 96–97% of
  core 1's time was inside `m68k_execute()`. Anything else on core 1 would
  steal from it.
- **SPI and I2C transfers block.** A full LCD frame is 480 × 320 × 2 bytes =
  300 KB. At 40 MHz that is about 61 ms of SPI time. The renderer waits on
  each DMA strip, and the touch task waits on I2C. On core 0 these waits
  cost the emulator nothing.
- **Touch must not depend on emulation speed.** The touch task samples at a
  fixed ~60 Hz. The emulation loop collects the result once per frame with
  `touch_consume()`, through a small spinlock-guarded queue. The gestures
  feel the same whether a frame took 5 ms or 50 ms.

The renderer reads Mac RAM **without any lock**. It only reads, and a torn
frame (half old, half new) is corrected on the next pass. This avoids any
synchronisation cost on the CPU side.

## The memory hierarchy, and why an interpreter cares

This is the most important section of the guide. Here is how the ESP32's
address space is used (addresses from the ESP32 Technical Reference Manual,
simplified):

```
 Data bus                                       What lives there
 ─────────────────────────────────────────────  ───────────────────────────────
 0x3F40_0000 ┌─────────────────────────────┐
             │ DROM: flash, read via cache │    .rodata: Apple ROM (512 KB),
             │ (up to 4 MB window)         │    disk image (1.44–2.88 MB), consts
 0x3F80_0000 ├─────────────────────────────┤
             │ PSRAM, read/write via cache │    Mac RAM 2 MB, Musashi jump +
             │ (4 MB window of the 8 MB)   │    cycle tables ~580 KB, LC II VRAM
 0x3FC0_0000 └─────────────────────────────┘    512 KB, disk write overlay
      ...
 0x3FFA_E000 ┌─────────────────────────────┐
             │ DRAM: internal SRAM (data)  │    stacks, heap, .bss, the 19.2 KB
 0x4000_0000 └─────────────────────────────┘    DMA line buffer for the LCD

 Instruction bus
 0x4007_0000 ┌─────────────────────────────┐
             │ SRAM0 used as the 2 × 32 KB │    (not usable by programs)
             │ flash/PSRAM caches          │
 0x4008_0000 ├─────────────────────────────┤
             │ IRAM: internal SRAM (code)  │    ISRs, FreeRTOS, m68k_execute,
             │ ~128 KB                     │    128 hottest opcode handlers,
 0x400A_0000 └─────────────────────────────┘    memory callbacks, lean fetch path
      ...
 0x400D_0000 ┌─────────────────────────────┐
             │ IROM: flash code via cache  │    all other code: ~2000 Musashi
             │                             │    handlers, device models, UI
             └─────────────────────────────┘
```

Four kinds of memory, with very different costs:

| Memory | Size here | Access | Cost |
|---|---|---|---|
| **IRAM** | ~128 KB (75–77.5 KB used by us) | Instruction fetch, direct | Fast: no cache in the way, so no misses |
| **DRAM** | ~365 KB free | Data, direct | Fast |
| **Flash** (code and `.rodata`) | 4 MB | Through the 32 KB cache | Fast on a hit; a miss reads a cache line over the 80 MHz QIO SPI bus |
| **PSRAM** | 4 MB mapped | Through the **same** 32 KB cache | Same as flash; several times slower than internal RAM on a miss |

The key fact: **flash code, flash data and PSRAM all share one 32 KB cache
per core.** Now think about what an interpreter does for every single 68k
instruction:

1. Fetch the opcode from Mac RAM (PSRAM).
2. Look up the handler in a 256 KB jump table (PSRAM) and the cycle cost in a
   64 KB cycle table (PSRAM).
3. Call the handler, whose code is in flash (cache) unless it is in IRAM.
4. The handler reads and writes Mac RAM (PSRAM) and maybe the ROM (flash).

All four streams compete for the same 32 KB. The jump table alone is eight
times bigger than the cache, and the Mac's working set is larger still. Every
miss stalls the core while a cache line comes over SPI.

That is why the biggest single speed-up in this project was not a clever
algorithm. It was **moving the hottest code into IRAM**, so that code
fetches stopped competing with data for the cache (−49% exec cycles, see
[chapter 4](04-performance.md)). It is also why some changes that are faster
on a PC are *slower* on the ESP32: making code bigger (inlining) means more
I-cache misses.

> **Rule of thumb for this board:** code size and placement matter more than
> instruction count.

### Why not put more in IRAM or DRAM?

- IRAM is only ~128 KB, and FreeRTOS, the drivers and interrupt handlers
  need part of it. With everything hot moved in, ~53 KB remain.
- The Musashi tables (~580 KB) and Mac RAM (2 MB) are far larger than all
  internal SRAM. They must live in PSRAM; `EXT_RAM_BSS_ATTR` places the
  tables there at link time. Without it, the link overflows `.dram0.bss` by
  about 434 KB.
- The ESP32 can execute code from flash, but **not from PSRAM**. A future
  translator (JIT/AOT) would have to put its code in flash or IRAM.

### The rev 1.0 PSRAM bug

The ESP32 rev 1.0 has a hardware bug: some sequences of loads and stores to
PSRAM through the cache can return wrong data. ESP-IDF works around it in
the compiler, so the workaround cannot be switched off on this chip. Two
strategies exist. We measured both on the board at steady Finder idle (ms
per 300 frames):

| Config | Idle time | Verdict |
|---|---|---|
| MEMW (memory barrier), DIO flash | 6936 ms | baseline |
| DUPLDST (duplicate loads/stores), DIO | 7023 ms (**−1.3%**) | rejected |
| MEMW, **QIO** flash | 6860 ms (**+1.1%**) | kept |

QIO uses four data lines to the flash instead of two, so code and ROM cache
misses are served faster. The gain is small because most misses hit PSRAM,
not flash.

## Flash: everything in one 4 MB chip

```
 0x000000 ┌──────────────────────────────┐
          │ bootloader, partition table  │
 0x009000 ├──────────────────────────────┤
          │ nvs        24 KB             │  (planned: PRAM persistence)
 0x00F000 ├──────────────────────────────┤
          │ phy_init    4 KB             │
 0x010000 ├──────────────────────────────┤
          │ factory app  0x3F0000        │
          │  ├─ code (.text)             │
          │  ├─ Apple ROM  512 KB        │  EMBED_FILES → .rodata
          │  └─ disk image 1.44–2.88 MB  │  EMBED_FILES → .rodata
 0x400000 └──────────────────────────────┘
```

The ROM and the disk image are linked into the firmware with ESP-IDF's
`EMBED_FILES`. They appear to C as plain arrays
(`_binary_LC_II_ROM_start`, `_binary_OS_750_boot_dsk_start`, ...). Nothing
is copied to RAM: the emulator reads them in place through the cache.

Consequences:

- **The disk is read-only.** Writes go to a copy-on-write overlay in PSRAM
  and are lost at power-off ([chapter 5](05-input-display-storage.md)).
- **The disk size is bounded by flash.** With the 2880 KB custom disk the
  firmware is about 0x3E18F0 of the 0x3F0000 partition, nearly full. That is
  also why the build uses `LC-II.ROM` (512 KB) rather than the Color Classic
  ROM (1 MB): switching saved 520 KB of flash.
- **ROM patches are applied on read.** The ROM in flash is never modified;
  the memory callbacks return patched words at the patched addresses
  ([chapter 2](02-emulated-macs.md)).

## Display and touch buses

| Device | Bus | Speed | Notes |
|---|---|---|---|
| ST7796S LCD | SPI2, DMA, write-only | 40 MHz | RGB565, BGR order, driven with `esp_lcd`'s ST7789 driver (compatible commands). ~61 ms per full frame |
| FT5x06 touch | I2C0 | 400 kHz | 7-byte read of registers 0x00–0x06: point count and 12-bit X/Y |

The LCD is the slowest link in the system: a full screen redraw can run at
only ~15 fps. The renderer avoids full redraws by sending only the 20-line
strips that changed ([chapter 5](05-input-display-storage.md)).

## Resource budget at a glance

| Resource | Used by | State (2026-10-21) |
|---|---|---|
| Flash app partition (0x3F0000) | Code + ROM + disk | ~0x3E18F0 with the 2880 KB disk; ~1.4 MB free with the 1.44 MB floppy |
| IRAM (~128 KB) | Hot Musashi code, memory callbacks | ~77.5 KB used, ~53 KB free |
| PSRAM (4 MB mapped) | Mac RAM 2 MB, tables ~580 KB, VRAM 512 KB (LC II), overlay 384/256 KB | Under 4 MB on both builds |
| Internal DMA RAM | LCD line buffer | 19.2 KB (480 × 20 × 2) |
| Core 1 | 68k emulation | ~97% in `m68k_execute()` when busy |

## Takeaways

- Core 1 runs only the Mac; core 0 handles everything that waits on a bus.
- One 32 KB cache sits in front of both flash and PSRAM. For an interpreter
  whose tables and guest RAM are all in PSRAM, **cache pressure is the
  dominant cost**.
- IRAM is small but miss-free. Spending it on the hottest 5% of code is the
  best trade on this chip.
- Flash holds code, ROM and disk together, and it is nearly full.

Next: [2. The emulated Macs](02-emulated-macs.md).
