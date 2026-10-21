# 6. Lessons learned

Emulators fail in confusing ways. The symptom (a crash, a freeze, a strange
screen) usually appears far from the cause. These stories come from the
project's records in
[`spec/performance-roadmap.md`](../../spec/performance-roadmap.md) and
[`spec/next-steps-plan.md`](../../spec/next-steps-plan.md). Each one ends
with the rule it taught.

## 1. Four bytes, two lost attempts: decode `4E FA` correctly

At offset `$2A` both V8 ROMs start with `4E FA 00 60`. During the second LC II
attempt, these bytes were read as a 6-byte `JSR.L $600000`. From that one
misreading, a whole theory grew:

- "the LC II ROM calls far addresses beyond 512 KB, so `LC-II.ROM` is a
  **truncated dump** of a 1 MB ROM";
- "a Start Manager must be resident in RAM at $600000 at reset", which led to
  an alias that mapped part of the ROM to $5FFFD2–$610000;
- a decision point: re-dump the ROM or switch to the Color Classic ROM.

The truth: `4E FA` is `JMP d16(PC)`, a 4-byte instruction. `4E FA 00 60` is
`JMP $8C(PC)`, a short jump inside the ROM. `4D FA 00 06 4E FA xxxx` is
Apple's "BSR6" idiom (load a return address into A6, then jump). The "far
calls" were short PC-relative jumps. The ROM was complete all along. Two
attempts' worth of conclusions were marked **superseded**. The disassembler
script had decoded it correctly; the human reading overrode it.

**Rule:** when a theory rests on one decoded instruction, check the encoding
in the reference manual, or better, watch the CPU execute it in an
instruction trace. The rule is now written into the project's ground rules:
*decode `4E FA` as `JMP d16(PC)`*.

## 2. "It boots" is not "it is right": the wrong CPU

An early note said the Color Classic used a 68EC000, so the V8 build ran
Musashi as a 68000. It even appeared to work: the Finder launched, idle was
73% of real time, boot took 157 s instead of 636 s. But the screen stayed
blank, and low-memory globals like `MemTop` and `ScrnBase` held the strange
value `$2704`.

The host instruction trace showed why. The V8 reset code opens with
`$F4AC`, a 68030 cache instruction. On a 68000 it faults. A Classic-era
"RECOVERY" hack caught the fault and jumped to `$0036`, and from then on the
boot ran on a corrupted path. `$2704` was a saved status register smeared
over the globals. Every 68000-mode "boot to Finder" had been an accident.

Both the Color Classic and the LC II are 68030 machines. With the right CPU
type, the boot took the real path.

**Rules:** check basic facts (which CPU?) against the code itself. Recovery
hacks that turn faults into jumps hide bugs; disable them when bringing up a
new machine, so real faults surface where they happen.

## 3. Classic rules leaking into the V8

The Classic build carried many rules that were true *for the Classic*. Copied
silently to the V8, they broke it:

| Classic rule | What it did on the V8 |
|---|---|
| The overlay clears on the first VIA ORA write (one-shot latch) | Fired at ROM `$47EA` and removed the ROM from under the running CPU. The CPU then walked through zeros |
| RECOVERY redirects for invalid PCs | Turned the `$F4AC` fault into a jump into nowhere (story 2) |
| Diagnostics reading `MemTop` and other globals at Classic addresses | Logged "MemTop = 0" for a V8 boot that was fine: the reads hit the wrong page |
| A PC histogram sized for Classic addresses | Overflowed its array with V8 PCs |
| The "first bad PC" crash detector | Still misfires on V8 ROM addresses |

The V8 rules came from the right sources instead: the device map from the
ROM's own decoder table, the overlay rule from MAME (it clears on the first
access to $A00000).

**Rule:** every machine-specific rule needs a gate (`#if !MAC_ROM_LC_II`)
and a source. When a new machine misbehaves, list the inherited rules first.

## 4. Measure on the target: inline RAM

`M68K_INLINE_RAM` copies the RAM fast path into each opcode handler, saving a
function call per memory access. On the PC host it was faster. On the board,
Finder idle took **3846M** exec cycles per 300 frames with it and about
**3200M** without it: about 16% *slower* with inlining. `M68K_INLINE_FETCH`
showed the same pattern (−8% when off).

The reason is the ESP32's 32 KB cache shared by code and data
([chapter 1](01-hardware-esp32.md)). Bigger handlers mean more instruction
cache misses, and each miss costs more than the call that was saved.

**Rule:** the host harness is for correctness and for finding hot spots.
Speed decisions are made on the board, with an A/B switch whose `0` keeps
the old behaviour.

## 5. Real-time % lies across machines

Three times a percentage pointed the wrong way:

- "LC II 17% vs Classic 91% idle" came from the broken 68000-mode boot of
  story 2. It said nothing about the real 68030 path.
- Musashi's 68030 cycle table charges about half the cycles of the 68000
  table. The LC II ran twice the instructions per frame (20.8k vs 10.2k) and
  showed a *lower* percentage for the same useful work.
- An idle Mac spins. Faster spinning raised the percentage without making
  anything the user does faster.

The fix was in the method: compare **exec cycles per 300 frames** for the
same workload, or **wall-clock seconds** for a task (boot to Finder) across
machines. And then fix the cause: the V8 cycle scale and the idle
fast-forward ([chapter 4](04-performance.md)).

**Rule:** before trusting a metric, ask what it is relative to.

## 6. Symptoms that meant something else

### The game that "froze"

Tapping in a full-screen game made the game disappear behind the Finder. It
looked like a freeze or a crash. It was System 7 working as designed: the
Process Manager routes a `mouseDown` to the window layer under the point. A
full-screen game owns no windows, so the click went to the Finder's layer,
and the Process Manager switched to the Finder. The fix was not in the
emulator core but in how clicks are posted: in game mode, clicks are posted
in the menu bar, which stays with the front application
([chapter 5](05-input-display-storage.md)).

### The boot disk that "went to the Trash"

On the LC II, around frame 3650, the boot floppy was ejected from the
desktop as if someone had dragged it to the Trash. Nobody had. The cause was
one drive sense line: the emulated SWIM returned 1 for **DISK SWITCHED**. The
`.Sony` driver's VBL task saw "disk switched" and posted a disk event every
30 ticks, until the event queue filled and the boot floppy was `_Eject`ed.
Returning 0 (as in MAME) fixed it.

### "The Finder is disk-bound"

During the Color Classic bring-up, the Finder plateaued at 15–19% of real
time. The working theory was that it was stuck decoding the floppy through
the SWIM. A counter was added to prove it. The real cause was the CPU mode
(story 2), not the disk.

### "68020 mode breaks the boot because of timing"

The first attempt to run the Classic ROM on a 68020 core crashed in
`_InitFonts`. The first explanation blamed two instructions; that was
corrected (they decode the same on both CPUs). The second blamed cycle
timing. Much later, a Musashi bug was found: `CPU_TYPE_IS_010_LESS` was a
constant 1, so 68020/030 decoded indexed addressing and exception frames the
68000 way. That is the more likely cause.

**Rule:** treat every explanation as a hypothesis until a trace or a counter
confirms it, and write the correction into the record when it turns out
wrong.

## 7. Small rules with big consequences

| Rule | What happened without it |
|---|---|
| `fast_slice` ≤ 4000 cycles | Deterministic crash around frame 5251: VIA timer underflows missed |
| `CrsrNew` only when the cursor moved | Cursor redraws every frame crashed the resource decompressor during boot |
| Clicks go to the event queue | Apps using `WaitNextEvent` never saw clicks set only in `MBState` |
| `MBState` written as a whole byte | The shutdown screen's Restart button waits for exactly `$00` |
| VIA IRB starts at `$31` (button down) on the Classic | The Classic boot needs it |
| Drive the IRQ line on every IFR/IER change | Stale interrupts re-entered ~14 times per frame (Classic); a zeroed `TimeDBRA` and a divide-by-zero Sad Mac (V8) |

## 8. The deterministic host harness

Most of these stories were solved on the PC, not the board. The host
harness runs the same emulator core:

| | Board | Host harness |
|---|---|---|
| 3000 frames | ~7 minutes | 0.5–0.8 s |
| Same input, same result | No (timing, touch) | **Yes, byte for byte** |
| Look inside | Serial log | Instruction trace, ring buffer of the last 4096 instructions, `--stop-pc`, `--watch` an address, RAM dumps, PNG screenshots |
| Profiling | Exec cycles per 300 frames | Per trap, per code page, per interrupt level |

Determinism is what makes the harness more than a fast debugger:

- The three root causes of the failed V8 attempts (CPU type, overlay rule,
  device map) were all found in one session with the instruction trace.
- Every native replacement was proven by comparing RAM dumps and screenshots
  with the switch on and off. That comparison found a **3-byte difference**
  that would have been invisible on the board: a slice hook that skipped one
  instruction after an interrupt had used up the slice.
- `tools/build_disk.py --prep` uses it to boot a new disk once and let the
  Finder build its desktop database.

**Rule:** debug logic on the host first; flash the board to confirm and to
measure.

## Summary

| Lesson | In one line |
|---|---|
| Decoding | Verify the encoding; trust a trace over a reading |
| Facts | Check the CPU, the map and the overlay rule against the ROM and a reference emulator |
| Inherited rules | Gate machine-specific code and list it first when something breaks |
| Target | Decide speed on the board; the PC can point the other way |
| Metrics | Know what a percentage is relative to |
| Symptoms | A freeze may be a layer switch; an eject may be one sense bit |
| Tools | A deterministic harness turns guesses into byte-exact comparisons |

Back to the [guide index](README.md).
