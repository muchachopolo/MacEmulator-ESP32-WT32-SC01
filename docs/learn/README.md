# Learn how it works

This guide explains how a €20 ESP32 touch-screen board runs a 68k Macintosh
with System 7.5.3. It is written for the curious hobbyist: someone who knows
some C and has met a microcontroller or a retro computer, but has never seen
this project's code.

The guide is about the **why**: why the code is shaped the way it is and what
we measured along the way. The [reference docs](../) list every address,
register and switch. The guide links to them and does not repeat them.

## Reading path

Read the chapters in order the first time. Each one builds on the previous.

| # | Chapter | You will learn |
|---|---|---|
| 1 | [The ESP32 board as a computer](01-hardware-esp32.md) | Two cores and what runs on each, the memory hierarchy (IRAM, DRAM, PSRAM, flash behind a 32 KB cache) and why it matters for an interpreter, what is stored in flash, the LCD and touch buses |
| 2 | [The emulated Macs](02-emulated-macs.md) | The Mac Classic (68000) and the LC II (68030, V8): address maps, chips, how the ROM boots, what is emulated at the hardware level and what is high-level (HLE), how the Mac OS ends up using a 480×320 screen |
| 3 | [CPU emulation with Musashi](03-cpu-emulation.md) | The interpreter loop, opcode tables, separate instruction fetches, the A-line hooks, time slices and interrupts, the 68030 fixes |
| 4 | [Making it fast](04-performance.md) | Performance as a case study: how we measured, every optimisation with its measured effect, what did not work on the ESP32, idle fast-forward, native replacements and how we proved them equivalent |
| 5 | [Input, display and storage](05-input-display-storage.md) | Touch to mouse (gestures, cursor, clicks, game mode), the display pipeline (dirty strips, lookup tables, DMA), the disk path (embedded image, .Sony hooks, write overlay, custom disks) |
| 6 | [Lessons learned](06-lessons-learned.md) | Debugging stories: misread opcodes, wrong CPU assumptions, rules that leaked between machines, measurements that lied, symptoms that meant something else |
| 7 | [The PC host harness](07-host-harness.md) | Hands-on tutorial: build variants, screenshots, scripted touch, memory/disk inspection, tracing a hang, profiling, byte-identical A/B testing |
| 8 | [How everything boots](08-boot-explained.md) | Diagrams: ESP32 boot (flash layout, bootloader, app start), the emulated Mac boot (overlay, ROM, System, Finder) and where the emulator steps in, the Raspberry Pi 2 bare-metal boot |
| 9 | [The Raspberry Pi 2 port](09-raspberry-pi-port.md) | Porting by shims, the display's real limit, four cores (68k, display, SD worker), disks on demand with write-back, a second drive, an ADB keyboard, speed work, and what went wrong |

Short on time? Read chapter 1, then chapter 4. Together they explain why the
emulator is fast enough to use.

## The big picture in one diagram

```
          ESP32 core 1                                ESP32 core 0
 ┌──────────────────────────────────┐      ┌───────────────────────────────┐
 │ mac_emu_run(): one loop per frame│      │ mac_video_task                │
 │  ├─ Musashi m68k_execute(slice)  │      │  reads Mac RAM / VRAM         │
 │  ├─ via_tick, Egret, SWIM        │      │  → RGB565 strip → SPI DMA     │
 │  ├─ VBL interrupt (60.15 Hz)     │      ├───────────────────────────────┤
 │  ├─ touch → cursor, clicks       │◄─────┤ touch sampler task (60 Hz I2C)│
 │  ├─ idle fast-forward            │      └───────────────────────────────┘
 │  └─ 60 fps throttle              │
 └──────────────┬───────────────────┘
                │ reads/writes
   ┌────────────▼────────────────────────────────────────────────┐
   │ PSRAM: Mac RAM 2 MB · Musashi tables · VRAM · disk overlay  │
   │ Flash: code · Apple ROM · System 7.5.3 disk image           │
   └─────────────────────────────────────────────────────────────┘
```

## Words used throughout

| Term | Meaning here |
|---|---|
| Frame | One emulated 60.15 Hz video frame. All timing is counted in frames (F2000 = frame 2000) |
| Exec cycles per 300 frames | ESP32 CPU cycles spent inside `m68k_execute()` over 300 frames. This is our main speed metric (lower is better) |
| Host harness | The same emulator core compiled for a PC (Linux/WSL), used for debugging and profiling ([host/README](../../MacintoshESPEmulator/host/README.md)) |
| HLE | High-level emulation: we imitate what a chip *does* for the OS, not the chip's internal program |
| A-line trap | A 68k opcode `$Axxx`. The Mac OS uses these as system calls |
| Low-memory global | A fixed RAM address the Mac OS uses as a variable (e.g. `Ticks` at `$016A`) |

## Where the facts come from

Every number in this guide comes from the project's measurement records:
[`spec/performance-roadmap.md`](../../spec/performance-roadmap.md) (history,
board A/B tables), [`spec/next-steps-plan.md`](../../spec/next-steps-plan.md)
(current state and results), and
[`host/prof_results/`](../../MacintoshESPEmulator/host/prof_results/)
(profiler reports). Where a number was not measured, the text says so.

Reference documentation:
[architecture overview](../architecture-overview.md) ·
[hardware](../hardware.md) · [memory map](../memory-map.md) ·
[peripherals](../peripherals.md) · [display pipeline](../display-pipeline.md) ·
[CPU core](../musashi-integration.md) · [build guide](../build-guide.md).
