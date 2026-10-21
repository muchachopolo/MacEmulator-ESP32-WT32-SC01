# 4. Making it fast: a case study

A 240 MHz ESP32 interpreting a 68k Mac starts out slow. The first Classic
build ran at 60–74% of real time when busy. The first correct LC II boot
took **246 s** to reach the Finder on the board. Today the Classic reaches
the Finder in about **63 s**, the LC II in about **115 s** (measured before
the native speed-ups), and both sit at 100% real time when idle.

This chapter tells how that happened: how we measured, what worked, what
did not, and why, on this specific hardware. Numbers come from
[`spec/performance-roadmap.md`](../../spec/performance-roadmap.md),
[`spec/next-steps-plan.md`](../../spec/next-steps-plan.md) and
[`host/prof_results/native_3b.md`](../../MacintoshESPEmulator/host/prof_results/native_3b.md).

## 1. Methodology

### Two machines for two questions

| Question | Where | Why |
|---|---|---|
| *Is it correct?* *Where does the 68k time go?* | **Host harness** (PC) | Same C code, single-threaded and deterministic: the same run gives the same RAM, byte for byte. 3000 frames in under a second. Per-instruction trace and profiler |
| *Is it faster on the ESP32?* | **Board** | Only the real cache, IRAM, PSRAM and flash give real numbers. The PC often gives the *opposite* answer (see section 3) |

The host harness ([README](../../MacintoshESPEmulator/host/README.md))
compiles `main/mac_*.c` and Musashi unmodified, against small shims for the
ESP-IDF APIs. Video is rendered between frames, touch comes from scripts
(`--tap`, `--swipe`), and `--vclock` makes the wall clock virtual so that
gestures do not depend on host speed.

### The profiler

`host_prof.c` (`make TRACE=1`) is called before each instruction. It reads
the opcode straight from the RAM array, so it does not disturb the
emulation. It attributes every instruction to:

- the **A-line trap** it runs inside (inclusive and self), with call sites;
- a 4 KB **code region** (ROM or RAM page) and its hottest PCs;
- **context**: interrupt handler or not, interrupt mask level.

### The board metric: exec cycles per 300 frames

The firmware logs a `SPEED` and a `PROFILE` line every 300 frames. `PROFILE`
reports the ESP32 cycles spent inside `m68k_execute()` (measured with
`esp_cpu_get_cycle_count()` around each slice). For an A/B test:

1. Build and flash from a clean `git worktree`, so no unrelated edit leaks
   in.
2. Let the Mac reach Finder idle without touching it.
3. Take the median of about 10 steady `PROFILE` lines.
4. Also record the boot-to-Finder time (the log marks when `CurApName`
   becomes "Finder").

**Lower exec cycles for the same workload = faster emulator.**

### Why "real-time %" misleads

Real-time % compares emulated cycles with the clock **we chose**
(`MAC_CPU_HZ` = 7.8336 MHz, 130,560 cycles per frame). It hides three traps:

1. **Different CPU tables.** Musashi's 68030 table charges about half the
   cycles of the 68000 table per instruction. A 68030 frame contains about
   twice the instructions, so the LC II showed a *lower* real-time % while
   doing the *same* useful work per second.
2. **Busy-waiting counts as work.** An idle Mac spins in wait loops. Running
   those spins faster raises the percentage without making anything the user
   does faster.
3. **Fast-forward and cycle scale change the meaning.** Once idle cycles are
   skipped (section 4), 100% means "throttled to real time", not "the CPU
   kept up".

So we compare **exec cycles per 300 frames** on the same workload and
machine, or **wall-clock seconds per task** (boot to Finder, open a folder)
across machines. Never real-time % across machines.

## 2. Where the time goes

**Phase 0** (board, Classic): 96–97% of core 1's time was inside
`m68k_execute()`; device models, video and the frame loop shared the other
3–4%. A PC histogram found Finder RAM code at ~80%, ROM memory-manager traps
and BlockMove growing to ~20% during Finder work.

**Step 3a** (host profiler, both machines) gave the decisive picture:

| Workload | Where the instructions go |
|---|---|
| Finder idle | ~90% inside `_WaitNextEvent`: the Process Manager's idle loop (OSEventAvail, trap `$ABF7`, a PPC call). One pass is ~390 instructions; 54 passes per frame on the LC II, 20 on the Classic |
| Dragging a window | ~97% in a **2-instruction loop**: `CMP.L Ticks,D0 / BHI` waiting for the next tick |
| Opening a menu | ~38% in MenuSelect, resource lookups |
| Boot | Resource loading and decompression; `Get1Resource` 22–45% self |
| Drawing traps (CopyBits, FillRect, text, BlockMove) | **< 0.5% each** |
| The A-line trap dispatcher | **15–17% of all instructions** |
| LC II vs Classic | 20.8k vs 10.2k instructions per frame, because of the 68030 cycle table |

Two lessons here. First, an idle Mac is not idle: it spins as fast as the
emulator lets it. Second, the obvious candidates for native code (drawing)
were cold. The trap dispatcher, the decompressor and the wait loops were
hot.

## 3. Making the interpreter itself faster

These changes make each emulated instruction cheaper. They are compile-time
switches, so each was A/B-tested on the board.

### Board A/B round (LC II, Finder idle, exec cycles per 300 frames)

All rows in this table are against the same baseline (4560M):

| Change | Exec cycles | Δ | Boot to Finder | Adopted |
|---|---:|---:|---:|---|
| Baseline | 4560M | — | 246 s | |
| `M68K_INLINE_FETCH` **OFF** | 4182M | **−8%** | 227 s | yes |
| `MUSASHI_HOT_IRAM` ON | 2307M | **−49%** | 172 s | yes |
| `MAC_MEM_CALLBACK_IRAM=1` | 4162M | **−9%** | 212 s | yes |
| `M68K_OP_INDEX` ON | 4426M | −3% | (run had touches) | no |

A separate round (different firmware state, its own baseline):

| Change | Exec cycles | Δ | Adopted |
|---|---:|---:|---|
| `M68K_INLINE_RAM` ON | 3846M | — | |
| `M68K_INLINE_RAM` **OFF** | ~3200M | **~−16%** | yes (OFF) |

### Why each one behaves the way it does

**Hot handlers in IRAM (−49%).** The host profiler ranked Musashi's handlers
by execution count. The top 128 cover about **94% of all executed
instructions**. The linker fragment `components/musashi/hot_iram.lf` places
`m68k_execute` and those 128 handlers in IRAM. Their code then never
competes with the jump table, the cycle table and Mac RAM for the shared
32 KB cache. Halving the cycles with zero change to the C code shows how
much the cache dominated.

**Memory callbacks in IRAM (−9%).** The same idea for
`m68k_read/write_memory_*`, called several times per instruction.

**Inlining is slower on the ESP32.** `M68K_INLINE_RAM` and
`M68K_INLINE_FETCH` copy the RAM fast path into every handler, saving a
function call. On a PC this is faster: it has megabytes of cache. On the
ESP32 it grows ~2000 handlers, most of which run from flash through the
I-cache, so code misses rise more than the call overhead falls. The PC said
"faster", the board said 8% and 16% *slower*. **Measure on the target.**

**Opcode index (−3%, not adopted).** `M68K_OP_INDEX` replaces the 256 KB
jump table + 64 KB cycle table with a 128 KB 16-bit index into small tables
in DRAM, halving the PSRAM bytes touched per dispatch. It helped only 3%, but
cost 128 KB of PSRAM and 10 KB of DRAM. It may be retested on top of hot
IRAM.

### Earlier and smaller wins

| Change | Effect | Where measured |
|---|---|---|
| `M68K_SEPARATE_READS` + lean fetch path in IRAM | **−17%** cycles (62% → 75% real time) | Board, Classic, early |
| PC read once per slice (was 8 `m68k_get_reg` calls) | not measured separately | Board |
| Flash QIO instead of DIO | **+1.1%** | Board |
| PSRAM workaround DUPLDST instead of MEMW | **−1.3%** (rejected) | Board |
| Bus-error emulation off (no register copy per instruction, no `setjmp` per slice) | −11% to −17% depending on the machine | Host |
| `MAC_DIAG=0` (boot diagnostics off the memory hot paths) | −4.5% to −8.5% | Host |
| Classic IRQ line driven immediately (`CLASSIC_VIA_IRQ_NOW`) | interrupt entries 14.0 → 1.2 per frame; Finder idle 3744 → 3629 instructions/frame | Host |
| VRAM 8/16/32-bit fast paths (LC II) | ~13% less host CPU while drawing | Host |
| Video dirty-strip skipping (`MAC_VIDEO_DIRTY`) | Static screen: 0 fps of SPI; core 0 stops competing for PSRAM bandwidth | Board |

The host-only numbers are directionally useful but not a promise for the
board; the inlining story above shows why.

## 4. Idle fast-forward: do not emulate waiting

The profile said an idle Finder spends ~90% of its time asking "is there an
event yet?". On a real Mac that is free: the CPU has nothing else to do. In
an emulator, every spin costs real ESP32 time, and it delays the moment the
display, touch and timers move on.

**Idea:** when the Mac is *provably* only waiting, skip the rest of the wait.
Advance device time (VIA timers, Egret, VBL) as if the CPU had spun, until an
interrupt that the CPU will take is pending. Then resume the CPU at the same
PC.

### The two detectors (`mac_emulator.c`)

1. **Process Manager idle loop.** System 7's scheduler, finding nothing to
   run, executes `_Trap $ABF7 ; MOVE.L A4,D0 ; BEQ.W loop` (bytes
   `ABF7 200C 6700 FF64`). The A-line hook sees each `$ABF7` from that exact
   site. A pass can only produce a different result if something changed:
   an interrupt (VBL → Ticks, Egret → ADB event, Time Manager) or a frame-end
   update (touch, disk). An **epoch counter** is bumped by each of those. If
   a whole pass ran and the epoch did not change, the next pass would be
   identical, so the slice ends and fast-forward starts.
2. **Tick-wait loops.** `CMP.L ($016A).W,Dn ; Bcc.S *-4` (compare `Ticks`,
   branch back). It can only exit when an interrupt changes `Ticks`. The
   frame loop checks the PC at the end of each slice.

### Fast-forward and throttle

```
 detector fires
      │
      ▼
 while frame cycles remain:
     if an interrupt above the SR mask is pending → stop, CPU resumes here
     advance devices by fast_slice cycles (via_tick), fire VBL at mid-frame
 frame end ─► touch, disk updates ─► throttle: sleep until 16.626 ms per frame
```

Device time advances in the same `fast_slice` chunks as normal execution, so
timers behave exactly as during a real spin, and the number of ticks per
frame does not change. The **frame throttle** (`MAC_FRAME_THROTTLE`) caps
the loop at one frame per 16.626 ms (60.15 Hz). Without it, cheap idle
frames would make the Mac's clock run fast.

### Results (board, 2026-10-21)

| Build | Before | After |
|---|---|---|
| LC II Finder idle | ~28% real time | **100%** (throttled), 60–65% of CPU cycles skipped |
| Classic Finder idle | — | **100%**, ~83% of cycles skipped (measured after the RTC/PRAM emulation removed the AppleTalk alert) |
| LC II boot to Finder | 246 s | **115 s** (with the switch winners and the V8 cycle scale) |

The V8 cycle scale ([chapter 3](03-cpu-emulation.md)) is part of the same
step: it stops the LC II from running twice the instructions per tick in its
wait loops.

## 5. Native replacements, proven equivalent

The next step replaced the hottest *Mac OS* code with C, inside
`components/musashi/mac_native.c`. The principle: **do not rewrite Mac OS**.
Replace only what the profiler shows is hot, and prove each replacement
equivalent on the host before trusting it.

| Replacement | Hook | Equivalence |
|---|---|---|
| **A-line trap dispatcher** (Classic System 7 RAM dispatcher, LC II ROM `$A099B0`) | After the A-line exception frame is built | **Byte-identical**: runs the dispatcher's own instructions as C, with Musashi's exact memory accesses, flags and cycle charges |
| **`dcmp` 3 resource decompressor** (System 7.5's LZ77 variant) | The dcmp entry is patched to the A-line word `$A89F`; the pre-hook runs C instead | **Output-exact**; takes zero emulated time, so the timeline changes |
| **Long ROM loops** (LC II RAM test, ROM checksum, wait loop, heap walk; Classic delay loop) | Start of each time slice | **Byte-identical** |

### How "exact" is achieved

The dispatcher replacement does not reimplement "dispatch a trap". It
executes **the same 68k instructions** the dispatcher contains, one by one,
written as C: the same reads and writes through Musashi's memory functions,
the same flag macros, the same cycle charges per opcode. Before each
instruction it repeats the interpreter's "cycles left?" check, so a time
slice ends on exactly the same instruction as when interpreted. What
disappears is the fetch, decode and dispatch overhead.

It is also defensive. The Classic dispatcher lives in RAM (System 7 installs
it), so on **every trap** the 218 bytes at the vector target are compared
with the known image. If anything patched or moved it, the interpreter runs
instead.

### How equivalence was tested

- **Matrix:** 4 configurations (LC II and Classic, each with the stock floppy
  and the custom disk) × 4 workloads (boot to F3000 and F8000, idle to
  F20000, a scripted gesture session to F56000).
- **Exact replacements:** RAM dumps and **208 screenshots** compared, switch
  ON vs OFF. The first run found a **3-byte difference** on one
  configuration at F20000. The cause: when an interrupt used up a slice's
  budget, the slice hook skipped the first instruction of the `do-while`
  loop. After the fix, all identical.
- **dcmp:** with `MAC_NATIVE_DCMP_CHECK=1`, each call also ran the 68k
  original and compared all RAM (except dead stack), all 16 registers, PC and
  flags: **2239 calls, 0 mismatches**. Offline, `make dcmp-test`
  decompressed every compressed resource on both disks (1143 resources,
  3.8 MB) with both the 68000 and the 68030 code: **2286 runs, 0
  mismatches**.

### Results

Interpreted instructions (host profiler):

| Machine | Workload | Off | Exact only (dispatcher + loops) | All on |
|---|---|---:|---:|---:|
| LC II | Boot to Finder | 64.4M (Finder at F5984) | 58.1M (−9.8%) | **12.5M (F1790, −80.6%)** |
| LC II | Menu | 31.3M | 27.8M (−11.3%) | −12.1% |
| Classic | Boot to Finder | 47.1M (F4906) | 46.7M (−0.8%) | **11.6M (F1165, −75.3%)** |
| Classic | Menu | 25.1M | 20.3M (−18.9%) | −18.7% |

The boot gain comes mostly from the decompressor: boot is dominated by
loading and decompressing resources, and the native version does it in zero
emulated time. On the board, the Classic (custom disk) reaches the Finder at
**~63 s instead of ~105 s**. The board LC II time with native code has not
been measured yet.

Code size is 18.2 KB of flash; none of it is in IRAM yet (a candidate A/B).

## 6. Summary of all speed-ups

| # | Speed-up | Mechanism | Measured effect | Where |
|---|---|---|---|---|
| 1 | Separate reads / lean fetch | Instruction fetches skip data-path intercepts after boot; IRAM | −17% cycles | Board, Classic |
| 2 | Bus-error emulation off | No register save per instruction | −11% to −17% | Host |
| 3 | `MAC_DIAG=0` | Diagnostics off memory hot paths | −4.5% to −8.5% | Host |
| 4 | QIO flash | Faster flash cache refills | +1.1% | Board |
| 5 | `M68K_INLINE_FETCH` off | Smaller handlers, fewer I-cache misses | −8% cycles, Finder 246 → 227 s | Board, LC II |
| 6 | `MUSASHI_HOT_IRAM` | 128 hottest handlers + execute loop in IRAM | **−49%**, Finder 246 → 172 s | Board, LC II |
| 7 | `MAC_MEM_CALLBACK_IRAM` | Memory callbacks in IRAM | −9%, Finder 212 s | Board, LC II |
| 8 | `M68K_INLINE_RAM` off | Smaller handlers | ~−16% vs ON | Board, LC II |
| 9 | Immediate IRQ line (Classic) | No stale interrupt re-entry | 14 → 1.2 IRQ entries/frame | Host |
| 10 | Dirty-strip video | Unchanged strips not converted or sent | 0 fps when static | Board |
| 11 | Idle fast-forward + throttle + V8 cycle scale | Skip provable waiting | LC II idle → 100%, 60–65% skipped; Classic 83% skipped; LC II Finder 115 s (with 5–8) | Board |
| 12 | Native dispatcher, dcmp, loops | Hot OS code in C, verified exact | −81% (LC II) / −75% (Classic) instructions to Finder; Classic Finder 105 → 63 s | Host / board |
| — | `M68K_OP_INDEX` | Smaller dispatch tables | −3%, not worth its memory | Board |
| — | DUPLDST PSRAM workaround | Alternative rev 1 fix | −1.3%, rejected | Board |
| — | `fast_slice` > 4000 | Fewer slice overheads | Crashes (VIA timer misses) | Board |

## 7. What remains

- **Boot workarounds keyed to frame numbers.** The lean fetch path switches
  on at frame 2000 (`mac_post_boot`). With the native code, the Classic
  reaches the Finder at about F1165, so the Finder starts on the slow path.
  Switching to "Finder launched" detection is estimated to bring the boot to
  20–30 s. Not done yet.
- **Resource-map search and handle traps** (HLock, HGetState, ...) are hot
  but have no zero-cost hook point yet.
- **Native code in IRAM**: an untested A/B.
- **Double-buffered video** (+19 KB DMA RAM) so rendering and SPI overlap.
- **Ahead-of-time translation** (plan step 3c): the boot disk is read-only,
  so the System, Finder and ROM code are fixed. Hot 68k blocks could be
  recorded on the host, translated to Xtensa on the PC, and linked into
  flash (the ESP32 cannot execute from PSRAM, and IRAM has ~53 KB left). At
  run time a block hash would select native code or the interpreter. High
  effort; a JIT into IRAM was parked for the same reasons.

## Takeaways

- Measure the right thing: exec cycles or wall-clock for a fixed task, not
  real-time %.
- Profile first. The hot spots (dispatcher, decompressor, wait loops) were
  not the ones we guessed (drawing).
- On this chip, **where code lives** beats **how much code runs**.
- The cheapest instruction is the one not emulated: skip provable waits, and
  replace hot routines with C that you can prove is identical.

Next: [5. Input, display and storage](05-input-display-storage.md).
