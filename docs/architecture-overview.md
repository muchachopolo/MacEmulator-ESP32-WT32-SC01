# Architecture overview

How the emulator is put together. Related documents:
[memory map](memory-map.md), [CPU core](musashi-integration.md),
[peripherals](peripherals.md), [display and touch](display-pipeline.md),
[hardware](hardware.md), [build guide](build-guide.md).

New to the project? Start with the [learning guide](learn/README.md), which
explains the hardware, the emulated Macs and the performance work step by
step; this page is the reference.

## Two machines, one codebase

| | Mac Classic | LC II (V8) |
|---|---|---|
| Build | `ROM_NAME "Classic.ROM"` → `MAC_ROM_LC_II=0` | `ROM_NAME "LC-II.ROM"` → `MAC_ROM_LC_II=1`, `MAC_V8_CUDA=0` |
| CPU | 68000 (Musashi) | 68030 (Musashi, PMMU instruction stub) |
| Screen | 1-bit framebuffer in Mac RAM, visible area patched to 480×320 | VRAM at $F40000, 1–16 bpp through the CLUT, 512×384 mode patched to 480×320 |
| ADB / clock / PRAM | VIA shift register bypass + RTC chip (`mac_rtc.c`) | Egret HLE (`mac_egret.c`) |
| Floppy | SWIM ISM emulation + .Sony Device Manager intercepts | .Sony Prime hook (`mac_v8disk.c`) |
| Clicks | C mirror of the ROM's PostEvent | Emulated ADB mouse button |

The Color Classic ROM (`MAC_V8_CUDA=1`, Cuda instead of Egret) uses the same V8
code and is kept as a reference. Most V8 code is shared; Classic-only code is
under `#if !MAC_ROM_LC_II`.

## Task model

```
            Core 0                                 Core 1
 app_main(): display_init, mac_emu_init
 mac_video_task  (renderer)                 mac_emu_run()  (emulation loop)
 touch sampler task (~60 Hz, I2C)             Musashi m68k_execute() slices
        │  touch_consume() once per frame ◄──     device ticks (VIA, Egret, SWIM)
        │  reads Mac RAM / VRAM            ──►    VBL, frame-end touch + disk updates
        ▼                                         idle fast-forward, throttle
 esp_lcd SPI DMA → ST7796S
```

`main.cpp` starts the emulation task on core 1 and the renderer plus the touch
sampler on core 0. The renderer reads Mac memory without locking; it only
reads.

## Frame loop (`mac_emu_run`)

One frame = one 60.15 Hz VBL = `MAC_CYCLES_PER_VBLANK` (7,833,600 / 60 ≈
130,560) cycles of **device time**.

1. Run the CPU in slices. Early boot uses 1-cycle slices, disk transfers 10,
   then 1000, and after frame 2000 `fast_slice` = 4000 (divided by
   `MAC_CYCLE_SCALE`). After each slice: `via_tick()`, Egret/SWIM ticks, IRQ
   update, PC-keyed boot workarounds. **`fast_slice` must stay ≤ 4000**: VIA T2
   periods are often 100–2000 cycles and longer slices miss underflows.
2. On V8, the CPU gets `MAC_CYCLES_PER_VBLANK / MAC_V8_CYCLE_SCALE` cycles per
   frame and each CPU cycle advances device time by the scale (default 2).
   Musashi's 68030 timings are about half the 68000's, so this keeps the LC II's
   instructions per tick close to the Classic's while timers stay real-time.
3. VBL fires mid-frame (VIA CA1; V8 also the slot VBL in pseudo-VIA2).
4. Frame end: touch report applied (cursor globals, button), V8 disk hook
   refreshed, low-memory checks.
5. **Idle fast-forward** (`MAC_IDLE_FF`): if the Mac is provably only waiting
   (Process Manager idle loop seen through the A-line hook with no interrupt
   since the previous pass, or a `CMP.L Ticks,Dn / Bcc *-4` loop), the rest of
   the spin is not emulated: device time advances in `fast_slice` chunks until
   an interrupt above the SR mask is pending. Ticks per frame are unchanged.
6. **Throttle** (`MAC_FRAME_THROTTLE`): at most one frame per 16.626 ms of wall
   clock, so a cheap idle frame does not run faster than real time.

`SPEED`, `PROFILE` and `IDLE-FF:` lines are logged every 300 frames.

## Source map

| File | Purpose |
|---|---|
| `main/main.cpp` | Entry point: display init, emulator init, tasks |
| `main/mac_emulator.c/.h` | Frame loop, ROM/disk setup, boot workarounds, touch apply, click posting, game mode, idle fast-forward |
| `main/mac_memory.c/.h` | Address decode for both maps, Musashi callbacks, ROM patches, .Sony intercepts |
| `main/mac_via.c/.h` | VIA 6522 (VIA1 on V8): timers, IRQs, overlay latch, ports |
| `main/mac_egret.c/.h` | Egret/Cuda HLE: ADB mouse, RTC, PRAM, IIC, 1 Hz ticks |
| `main/mac_rtc.c/.h` | Classic RTC/PRAM chip on VIA port B |
| `main/mac_iwm.c/.h` | IWM/SWIM: sense lines, ISM registers, MFM byte stream |
| `main/mac_v8disk.c/.h` | V8 .Sony Prime replacement |
| `main/mac_disk_overlay.c/.h` | Copy-on-write disk overlay in PSRAM |
| `main/mac_v8video.c/.h` | V8 VRAM, VDAC/CLUT, pseudo-VIA2, ROM video patch |
| `main/mac_video.c/.h` | Renderer (Classic 1-bit and V8 CLUT paths), dirty strips |
| `main/mac_touch.c/.h` | Touch gestures and sampler task |
| `main/mac_scsi.c/.h` | NCR 5380 with an empty bus |
| `main/mac_scc.c/.h` | Z8530 stub |
| `components/musashi/` | Musashi core, `m68kmmu.h` PMMU stub, `mac_native.c` native speed-ups, `hot_iram.lf` |
| `components/display/display.cpp` | ST7796S (esp_lcd SPI) and FT5x06 (I2C) |
| `host/` | x86 harness: shims, `host_main.c`, `host_prof.c` ([README](../MacintoshESPEmulator/host/README.md)) |

## Memory placement

| Region | Contents |
|---|---|
| Flash (4 MB, app partition 0x3F0000) | Code, ROM image (512 KB, or 1 MB Color Classic), disk image (1.44–2.88 MB) in `.rodata` via `EMBED_FILES` |
| IRAM | `m68k_execute` + 128 hottest opcode handlers, memory callbacks, lean fetch path (~77.5 KB used) |
| PSRAM (~4 MB addressable) | Mac RAM 2 MB, Musashi jump/cycle tables ~580 KB, V8 VRAM 512 KB, disk overlay pool 384/256 KB |
| Internal DMA RAM | 19.2 KB render line buffer |

The disk image stays in flash; it is not copied to PSRAM. Writes go to the
overlay ([peripherals](peripherals.md#disk-overlay)).

## Boot path

**Classic:** reset vectors through the overlay → first VIA ORA write clears the
overlay (one-shot latch) → memory/hardware tests (with workarounds) → SWIM
init → boot blocks and System file read through .Sony intercepts → INITs →
`_InitWindows` → Finder. From frame 2000 (`mac_post_boot`) the lean
instruction-fetch path is used.

**LC II:** 68030 reset → overlay clears on the first access to $A00000 → Egret
handshake (HLE) → RAM sizing (2 MB, $FF above) → video (sense code 2, VPBlocks
patched to 480×320) → .Sony Open, then reads through the Prime hook → System
7.5.3 → Finder.

### Classic boot workarounds (history)

The Classic boot needed targeted workarounds where the emulator lacks hardware
or where early emulation bugs had to be bridged. Most are still in the code
(Classic only) and keyed to ROM addresses:

| Workaround | Why |
|---|---|
| Overlay one-shot latch on the first ORA write | Later ORA writes (sound, screen select) must not re-map ROM |
| D6 = 0 at $401CF0/$401D80/$401D96 | Memory test result (safety net) |
| Serial debug monitor escape to $401F2C | The ROM's SCC monitor loop has no exit |
| SCSI polling bypass at $41A44A, SCSI reads $00 during boot | No SCSI devices; prevents a SCSI Manager allocation that crashed INIT loading |
| .Sony Device Manager intercepts ($40315C read, ctl/stat), DM-spin rescue ($4031BE), `.Sony _Write` ($4031CC) | Disk I/O served from flash/overlay |
| ADB completion flag $08446C, PB4/PB5 high, SR interrupt emulation | No ADB transceiver |
| StartUpScreen BNE→BRA | Missing DSAT resource |
| UNIMP handler at $400768 | Unimplemented traps return cleanly |
| SCC RR0 Sync/Hunt bit | LocalTalk INIT polls it |
| ROM checksum and RAM test skips (Mini vMac approach) | ROM is patched; RAM test is slow |

Debugging history in brief: overlay latch → "?" floppy icon → SWIM ISM sector
reads → ADB/LocalTalk/StartUpScreen stalls → Finder (April 2026) → speed work
and touch trackpad → event-queue clicks → 68030/V8 bring-up, Egret, video,
native 480×320 (October 2026) → native speed-ups, writable overlay, game mode.
Full record: [`spec/performance-roadmap.md`](../spec/performance-roadmap.md).

## Background reading

- 68000: *M68000 Family Programmer's Reference Manual* (Motorola). 68030:
  *MC68030 User's Manual* (PMMU, cache instructions).
- Mac hardware: *Guide to the Macintosh Family Hardware* (Apple, 2nd ed.).
- Mac OS internals: *Inside Macintosh* (low-memory globals, trap dispatch,
  Memory/Resource/Event Managers, Device Manager).
- ESP32: ESP-IDF Programming Guide and the ESP32 Technical Reference Manual
  (PSRAM cache, SPI DMA, IRAM).
- Reference emulators: Mini vMac (Classic ROM patches), MAME (V8, Egret, Cuda).

## Glossary

| Term | Meaning |
|---|---|
| A-line trap | `$Axxx` opcode; Mac OS system call via the trap dispatcher |
| ADB | Apple Desktop Bus (keyboard/mouse) |
| Egret / Cuda | 6805 microcontrollers on V8 Macs: ADB, RTC, PRAM, power (LC II = Egret, Color Classic = Cuda) |
| HLE | High-level emulation: behaviour emulated, not the MCU's code |
| ISM | SWIM's MFM register interface |
| Overlay | ROM mirrored at address 0 after reset |
| PRAM | Parameter RAM (settings kept by the clock chip) |
| PSRAM | External SPI RAM on the ESP32-WROVER module |
| V8 | Apple's LC II / Color Classic system chip (memory controller + video) |
| VBL | Vertical blank interrupt, 60.15 Hz |
| VIA | MOS 6522 Versatile Interface Adapter |
| VPBlock | Slot Manager video parameter block (mode size, rowBytes, depth) |
