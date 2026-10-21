# 3. CPU emulation with Musashi

The emulator does not translate 68k code into Xtensa code. It **interprets**
it: for every 68k instruction, C code decodes the opcode and performs its
effect. The interpreter is [Musashi](https://github.com/kstenerud/Musashi)
by Karl Stenerud, a portable 680x0 core used by many emulators (MAME among
them). A patched copy lives in `MacintoshESPEmulator/components/musashi/`.

Reference: file list, switches and fixes are in
[`docs/musashi-integration.md`](../musashi-integration.md). This chapter
explains how the pieces work at run time.

## Generated handlers and two big tables

Musashi does not contain a hand-written `switch` over opcodes. Instead,
`m68k_in.c` describes each instruction pattern once, and a generator
(`m68kmake`) expands it into about 2000 small C functions, one per
instruction × addressing-mode combination (`m68k_op_moveq_32`,
`m68k_op_move_32_d_di`, ...).

At start-up, Musashi fills two tables indexed by the full 16-bit opcode:

| Table | Entries | Size | Where |
|---|---|---|---|
| `m68ki_instruction_jump_table` | 65,536 handler pointers | 256 KB | PSRAM |
| `m68ki_cycles[cpu][opcode]` | 65,536 cycle counts per CPU type | 64 KB each (~580 KB with the jump table) | PSRAM |

They are far too big for internal RAM. `EXT_RAM_BSS_ATTR` (written into the
tracked `m68k_in.c`, so every regeneration keeps it) places them in PSRAM.

## The execute loop

Stripped of the parts that are compiled out, `m68k_execute()` in
`m68kcpu.c` is:

```c
int m68k_execute(int num_cycles)
{
    SET_CYCLES(num_cycles);              // budget for this time slice
    m68ki_check_interrupts();            // take a pending IRQ first
    if (!CPU_STOPPED) {
        if (!M68K_SLICE_START_HOOK() || GET_CYCLES() > 0)   // native ROM loops
        do {
            REG_PPC = REG_PC;
            REG_IR  = m68ki_read_imm_16();               // fetch opcode
            m68ki_instruction_jump_table[REG_IR]();      // run its handler
            USE_CYCLES(CYC_INSTRUCTION[REG_IR]);         // charge its cost
        } while (GET_CYCLES() > 0);
    }
    return cycles_used;
}
```

Per 68k instruction this costs: an opcode fetch (a memory callback), two
table loads from PSRAM, an indirect call, and the handler's own work (more
fetches for extension words, data reads and writes through the memory
callbacks). [Chapter 1](01-hardware-esp32.md) explains why every one of
these touches the shared 32 KB cache, and [chapter 4](04-performance.md)
what was done about it.

Several of Musashi's options are **turned off for speed** in `m68kconf.h`:
prefetch emulation, address-error emulation (a `setjmp` per call and an
alignment check per access), trace mode, function codes, the PC monitor and
bus-error emulation. Bus-error support copied all 16 registers before every
instruction, just in case. Nothing in this emulator raises a bus error
(unmapped reads return `$FF`), so it was pure overhead.

## Memory callbacks: the bridge to the Mac

Musashi calls C functions for every memory access:
`m68k_read_memory_8/16/32` and `m68k_write_memory_8/16/32`, implemented in
`main/mac_memory.c`. They hold the address decoder from
[chapter 2](02-emulated-macs.md): fast paths for RAM, ROM and VRAM first,
then a slower `mem_read_byte()` / `mem_write_byte()` for devices.

### Separate reads: a lean path for instruction fetches

With `M68K_SEPARATE_READS` on, Musashi uses a **different** set of callbacks
for instruction and immediate fetches (`m68k_read_immediate_16/32`,
`m68k_read_pcrelative_*`). This split exists because the data path collected
boot diagnostics and intercepts over time, while instruction fetches, the
most frequent accesses of all, need almost none of it.

The fetch callbacks live in IRAM and work in two modes:

- **During boot** (`mac_post_boot` false), they call the full data callbacks,
  because some boot workarounds hook instruction fetches.
- **After frame 2000** (`mac_post_boot` true), they take a lean path: a RAM
  bounds check and two byte loads, or a ROM read with a few fixed address
  checks.

This split gave a **17%** speed-up early in the project (62% → 75% of real
time on the Classic, at the time).

### Fetch intercepts: changing what the CPU sees

The lean fetch path also shows a technique used throughout the emulator.
To run C code at a given ROM address, you do not need a hook in the CPU
core. You **intercept the instruction fetch** at that address:

```c
// mac_memory.c, inside m68k_read_immediate_16() (Classic build)
if (address == DM_READ_ENTRY)        // .Sony _Read handler at $40315C
    return read16_dm_read();         // does the whole read in C, then
                                     // returns $4E75 (RTS) as the "opcode"
```

`read16_dm_read()` reads the parameter block that A0 points to, copies the
sectors from the disk image into Mac RAM, fills in the result fields, sets
D0 = 0, and returns the opcode `RTS`. The CPU executes that `RTS` and returns
to the caller as if the driver had finished. If the request is not for the
floppy, it returns the real ROM word and the driver runs normally. The UNIMP
trap handler and the Classic disk control/status intercepts use the same
trick. It is cheap: one compare on the fetch path, and nothing at all at
other addresses.

## The A-line hooks

Mac OS system calls are **A-line traps**: opcodes `$A000`–`$AFFF`, which a
68k CPU treats as "unimplemented instruction" and sends to an exception
vector. The OS's trap dispatcher then looks up the trap number in a table and
jumps to the handler.

This path is a perfect place for emulator hooks: every system call passes
through it, and it is *not* on the per-instruction path. Three hooks were
added to `m68ki_exception_1010()` in `m68kcpu.h`:

```
 $Axxx opcode
     │
     ├─ M68K_ALINE_PRE_HOOK(ir)   → mac_native_aline_pre(): if ir is the
     │                              patched dcmp word, run the native
     │                              decompressor and skip the exception
     ├─ M68K_ALINE_HOOK(ir, pc)   → mac_aline_hook(): spot the Process
     │                              Manager idle loop (idle fast-forward)
     ├─ build the exception frame, jump through the A-line vector
     └─ M68K_ALINE_POST_HOOK()    → mac_native_aline(): if the vector target
                                    is a known dispatcher, run it in C
```

Plus one hook per **time slice**: `M68K_SLICE_START_HOOK()` runs known long
ROM loops natively when a slice starts inside one. What these hooks do and
how they were verified is in [chapter 4](04-performance.md).

## Time slices and why they stay small

The emulator runs the CPU in **slices**. After each slice it advances the
devices (VIA timers, Egret, SWIM byte clock) by the cycles just executed and
updates the interrupt line. One frame (`MAC_CYCLES_PER_VBLANK` = 7,833,600 /
60 = 130,560 cycles) is many slices:

| Phase | Slice (CPU cycles) | Why |
|---|---|---|
| Frames 600–640 | 1 | Single-step for a boot diagnostic (trap ring buffer) |
| Floppy transfer in progress | 10 | The SWIM byte stream must be timed closely |
| Up to frame 2000 | 1000 | Boot intercepts check the PC after each slice |
| After frame 2000 | `fast_slice` = 4000 | Least loop overhead |

On the LC II the CPU-cycle numbers are divided by `MAC_CYCLE_SCALE` (2), so
the device-time granularity stays the same (see below).

**`fast_slice` must not exceed 4000 cycles.** This was tested exhaustively
up to 40,000. Larger slices crash deterministically (around frame 5251, with
the PC running into device space). The reason: the VIA's timer 2 is often
programmed with periods of 100–2000 cycles. Interrupts are only raised
between slices, so with long slices the timer underflows too late or
several underflows merge into one. The Time Manager and the ROM's delay
calibration then go wrong.

Later, the VIA model gained **mid-slice interpolation** (a timer read
computes the counter from the cycles executed so far in the slice) and an
E-clock rate (CPU/10). That made timer *reads* exact, but interrupts still
fire at slice boundaries, so the 4000 limit remains.

## Interrupts

The Mac uses **autovectored** interrupts: the interrupt level selects the
vector directly. `mac_int_ack_handler()` answers Musashi's acknowledge
callback.

```
 VIA (IFR & IER) ──► level 1 ─┐
 pseudo-VIA2 slot VBL (LC II) ──► level 2 ─┼─► m68k_set_irq(level)
                                            │
 m68ki_check_interrupts(): at slice start, and whenever SR changes
 (e.g. the OS lowers its mask with MOVE #$2000,SR)
```

So an interrupt that becomes pending mid-slice is taken at the latest at the
next slice start (≤ 4000 cycles, about half a millisecond of Mac time).

One bug worth knowing: the Classic used to update the IRQ line only between
slices. After the OS cleared the VIA flag, the CPU still saw the stale line
and re-entered the interrupt handler. That happened about **14 times per
frame**. Driving the line on every IFR/IER change (`CLASSIC_VIA_IRQ_NOW`)
brought it down to 1.2 per frame. Finder idle went from 3744 to 3629
instructions per frame on the host. On the V8, a stale IRQ had been worse:
it zeroed `TimeDBRA` during calibration and caused a divide-by-zero Sad Mac.

## Cycle timing and the V8 cycle scale

Musashi charges each instruction the cycle count from the table of the
selected CPU type. The **68030 table is about twice as fast** as the 68000
table: 6.2 cycles per instruction on average, against 12.8. With the same
130,560 cycles per frame, the LC II therefore ran about **twice as many
instructions per frame** as the Classic (20.8k vs 10.2k in the profiler),
even when it had no more work to do. Busy-wait loops simply spun twice as
often.

`MAC_V8_CYCLE_SCALE` (default 2) fixes this. The LC II CPU gets half the
cycles per frame, and each CPU cycle advances device time by two. Timers,
VBL and the Mac clock stay in real time, and the LC II runs about as many
instructions per tick as the Classic.

## Getting the 68030 right

The LC II needs a 68030. Three changes made Musashi's 68030 mode work for it:

1. **`CPU_TYPE_IS_010_LESS` fix.** With 68020 emulation compiled in, this
   macro fell back to a constant `1`. So the 68020/030 decoded **indexed
   addressing** (full-format extension words) and **exception frames** the
   68000 way. The early "68020 mode breaks the Classic boot" failure was at
   first blamed on cycle timing; this bug is the more likely cause. The macro
   is now an exact test of the CPU type.
2. **PMMU stub** (`m68kmmu.h`). The MMU instructions are decoded with correct
   operand lengths and accepted. PTEST does a software table walk. There is
   no per-access translation, because the ROM's MMU tables map exactly what
   24-bit masking maps (verified). See [chapter 2](02-emulated-macs.md).
3. **Cache instructions** are accepted. The LC II reset code opens with one
   (`$F4AC`), which would fault on a 68000.

The 68030 also needs 68020 emulation enabled (it is a superset). With 030 on,
the generated table references the FPU handlers, so `m68kfpu.c` and SoftFloat
are linked, although these Macs have no FPU. The CPU type is chosen at run
time (`M68K_CPU_TYPE_68000` or `_68030`), so one generated table serves
both builds.

## Takeaways

- Musashi is a table-driven interpreter. Each 68k instruction costs a fetch,
  two PSRAM table loads, an indirect call and the handler.
- Instruction fetches have their own lean callbacks. A fetch intercept can
  replace a ROM routine with C by returning `RTS` as the opcode.
- The A-line exception path and the slice start are the natural hook points:
  frequent enough to matter, never per instruction.
- Slices stay ≤ 4000 cycles because device interrupts are raised between
  slices.
- The 68030's cheaper cycle table needs a cycle scale; otherwise the LC II
  just spins twice as fast in its wait loops.

Next: [4. Making it fast](04-performance.md).
