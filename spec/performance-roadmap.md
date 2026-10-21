# Performance roadmap — Macintosh ESP32 emulator

> **Historical record.** This file keeps the history and measurements; the current plan lives in [`next-steps-plan.md`](next-steps-plan.md).

Spec written 2026-10-16. Purpose: durable plan so a future session can resume without
re-deriving context. Update the Status line and tick boxes as work lands.

**Next steps: see `spec/next-steps-plan.md` (2026-10-21).**

Status (2026-10-20): Phase 0 done. Phase 1 rolled back. Phase 2 IN PROGRESS on
Color-Classic.ROM. Attempt 4 (2026-10-20) found three emulator bugs that sank every
earlier V8 attempt (wrong CPU, wrong overlay rule, Classic RECOVERY hack). Attempts 2/3
were built on a misdecode and are superseded. The boot now runs on the real 68030 path
and is blocked on **Egret** (ADB/RTC/PRAM microcontroller), which is being implemented
on the host harness. **Plan of record: once V8 boots, switch the build to LC-II.ROM.**
It is complete (512KB) and saves 512KB of flash vs the 1MB Color Classic ROM. Speed is
identical (see "ROM choice reassessment"). The last fully working build is still
commit c9abaef (68000 + Classic.ROM).

## Baseline (as of 2026-10-16)

- System 7.5.3 Finder boots and runs stably; 60–74% real-time steady state, ~91% peak idle.
- 96–97% of time inside `m68k_execute()`; overhead 3–4% (PROFILE log lines).
- Toolchain: ESP-IDF **v6.1** at `C:\esp\v6.1\esp-idf` (v6.0 no longer exists on disk;
  `build.bat` already updated). Binary 2.48MB, flash partition 4MB.
- Musashi is built in **68000 mode**: `m68kconf.h` has `M68K_EMULATE_020 OPT_OFF`,
  `M68K_EMULATE_030 OPT_OFF`; `mac_emulator.c:165` sets `M68K_CPU_TYPE_68000`.
  The Classic ROM is 68020 code — any 68020-only instruction or addressing mode hits the
  UNIMP stub at $400768. This is a correctness risk, not just a speed issue.
- Open anomaly: `MACPC` log shows PC in $A0008FA0 / $A000AA72 (SCC device space) in 2 of
  8 samples. Finder code never lives there; lean fetch path likely aliases it into RAM.
  Must be understood before any JIT work.
- Monitor tool: `tools/serial_watch.py` (COM3 serial stream → `tools/serial_watch.log`).
- Reference docs: `E:\Personal\mac_rom` — bit-identical 68000 reconstruction of the
  128K/512K/Plus ROMs with annotated `include/*.inc` (traps, low-mem, sysequ, sonyequ).
  Plus-era, NOT the Classic ROM: use as documentation for intercept work, never as the
  base ROM for System 7.5.3.

## Why "new ROM from scratch" is not the lever

The ROM is not the bottleneck. Finder + System 7 code (hundreds of KB of 68000
machine code) dominates execution and must run on an emulated CPU regardless of ROM.
A custom ROM buys boot-time only. The speed levers, in order of value/risk:

1. 68020 mode (correctness + cycles/instruction) — keep Classic ROM.
2. ~~68030 mode + LC II ROM (bigger win)~~. **Corrected 2026-10-20:** a 68030 machine is
   not a speed lever. It is a capability lever (colour, >4MB RAM, newer System
   versions). See "ROM choice reassessment".
3. Native ESP32 intercepts for hot ROM routines (the "use ESP32 hardware" idea, done at
   the right layer).
4. JIT 68000→Xtensa into IRAM (parked; only if 1–3 leave it too slow).

---

## Phase 0 — Profiling + anomaly hunt (do first, cheap)

Goal: a PC histogram that ranks routines by cycle share, and a counter that proves the
SCC-space PC anomaly.

Steps:
1. In `mac_emulator.c`, add a static histogram: 640 buckets × uint32 (4KB pages: 512 RAM
   + 128 ROM). Sample PC every 64 frames at the existing per-frame check site.
   Bucket: RAM `(PC & 0x1FFFFF) >> 12`; ROM `512 + (PC >> 12 & 0x7F)`; anything else →
   `bad_pc_count++`.
2. Every 3000 frames, log top 10 buckets as `PC-HIST: page=<addr> share=<%>` plus
   `BADPC: <count>`.
3. Run `serial_watch.py` for a few minutes of Finder idle + one scripted interaction
   (open a folder, drag).

Done when: top-10 routine list is stable across runs and BADPC count is quantified.
Rollback: remove the block; it is self-contained.

### Phase 0 results (2026-10-16, Finder idle, 110 samples)

Top pages (cumulative): $00A000 30%, $008000 20%, $08C000 19%, $009000 7%, $007000 5%,
$005000 3%, $00C000 2%, $40A000 2%, $40B000 2%, $07D000 1%. **BADPC: 0 of 110.**

Conclusions:
- The SCC-space PC anomaly is benign: the PC register's upper byte is dirty (Musashi
  `get_reg` returns unmasked; e.g. $A0008FA0 masks to $008FA0, a hot Finder page).
  Low 24 bits are always valid RAM/ROM. JIT precondition holds — no chase needed.
- Finder RAM code (pages $5000-$C000, $7D000-$8C000) dominates at ~80% cumulative;
  ROM share varies by activity: ~5% in the first windows but ~25% in the frame 9000–12000
  window, concentrated in $40A000/$40B000 — which per the boot log are the memory
  manager trap handlers ($40A5EE NewPtr, $40A60A DisposPtr, $40A666 NewHandle…) and
  BlockMove ($40B28C). Those are exactly Phase 3's intercept targets and they are
  genuinely hot during Finder work. Phase 3 ceiling: workload-dependent, up to ~25%.
- Report 4 (157 samples): $00A000 21%, $008000 14%, $08C000 13%, $007000 8%, $40A000 8%,
  $009000 5%, $00C000 3%, $40B000 3%, $005000 2%, $061000 2%. BADPC 0/157.
- Report 5 (204 samples): $00A000 16%, $40A000 11%, $008000 10%, $007000 10%, $08C000 10%,
  $40B000 5%, $009000 3%, $00C000 3%, $060000 3%, $410000 2%. BADPC 0/204.
  In the 12000–15000 window ROM samples were ~47% (cumulative ROM ~20%): $40A000 +10,
  $40B000 +6, $410000 +6 (page to identify via dis68k.py — likely ROM graphics library).
  Phase 3 ceiling revised: ~20% cumulative and rising with Finder activity, not 5%.
- Sampling is PC-per-frame, not cycle-weighted: hot tight loops inside a page are
  undercounted if they span few frames. Good enough for ranking; revisit if Phase 4 starts.

## Phase 1 — 68020 mode, Classic ROM unchanged

Goal: correct emulation of the Classic ROM's own instruction set; UNIMP stubs stop
masking illegal instructions. Expect similar or modestly better real-time %; the win is
correctness, and it gates Phase 2.

Steps:
1. `components/musashi/m68kconf.h`: `M68K_EMULATE_020 OPT_ON` (leave 030 OFF).
2. Regenerate Musashi ops (conf change invalidates committed `m68kops.c/h`):
   `gcc -o m68kmake m68kmake.c && ./m68kmake . m68k_in.c` in `components/musashi/`.
3. `mac_emulator.c:165`: `M68K_CPU_TYPE_68020`.
4. Rebuild (`build.bat build` → `flash`), run monitor.
5. Verify: boot reaches Finder (`FinderName="Finder"` marker), UNIMP hits drop to ~0
   (add a one-line UNIMP hit counter at the trap intercept if not already present),
   no new crashes, SPEED recorded.

Done when: Finder boots with UNIMP≈0 and no regression in stability.
Risks: generated tables grow — check `size` output; PSRAM headroom is ~1.4MB, tables
were 580KB, expect well under budget. `fast_slice ≤ 4000` constraint unchanged.
Rollback: git revert the three edits; committed 68000-mode ops files are in the tree.

### Phase 1 attempt (2026-10-16) — FAILED, rolled back

What was done: `M68K_EMULATE_020 OPT_ON` + `M68K_CPU_TYPE_68020`, ops regenerated,
built (0x26e8d0, 38% free), flashed. Boot crashed: `CRASH DETECTED F918: PC=$4E2010`
inside `_InitFonts`; device showed the Welcome banner with no progress bar.

What actually diverged — **cycle timing**, not semantics at the sites first blamed.
`MOVE.W $C(A7),-(A7)` ($41A006, `3F2F 000C`) and `LEA $419FFE(PC),A0` ($41A028,
`41FA FFD4`) are plain 68000 modes (`d16(A7)`, `d16(PC)`) — they execute identically
in both builds, so they are NOT the cause (earlier claim was wrong; corrected here).
The 020-mode cycle table costs some instructions differently, and the boot bypasses
are timing-tuned: under 020 timing the SCSI countdown (DBF loop at $41A84A, MULU
#$0113) swallows the BOOT3-F633..F642 markers, the ADB clock read fails (BD-WRITE
$0210←00/$0211←01 vs Finder's 80/23), and the flow diverges upstream of the PACK
GetRes — an extra 0x28 word on the stack makes the intercept read type=40, name='??PA'.
Disk I/O (DM-READ #0..#16) stayed byte-identical.
Open question: whether any genuinely 020-only instruction (indexed EA, CALLM/RTM,
loop ops) executes in the boot path at all. The cheap probe answers it: `M68K_ILLG_HAS_CALLBACK
OPT_SPECIFY_HANDLER` in a 68000 build, log opcode+PC per illegal opcode.

Consequence: Phase 1 needs the same intercept re-derivation discipline as Phase 2 step 4,
against the Classic ROM's true 020 flow — not a two-line flip. Cheap next attempt if
revisited: `M68K_ILLG_HAS_CALLBACK OPT_SPECIFY_HANDLER` in a 68000 build to enumerate
every masked opcode site, then fix the intercepts that depend on masked semantics.

Rollback executed: m68kconf.h + mac_emulator.c reverted, 68000 build rebuilt (0x260ef0,
40% free), flashed, Finder boots again (`FinderName="Finder"` at t=73s, SPEED 71–83%).

Build gotchas found along the way (keep for future regenerations):
- `components/musashi/m68kops.c` is **gitignored** — regeneration replaces the working-tree
  copy, which is the only place the `EXT_RAM_BSS_ATTR` on the two big tables lives. Without
  it the link overflows `.dram0.bss` by ~434KB. Fixed at the source: the attribute now sits
  in tracked `m68k_in.c` (TABLE_HEADER section) so every regeneration carries it.
- `m68kmake` does not read `m68kconf.h`; generated ops are config-independent (`#if` guards
  select at compile time). A conf flip alone does not require regeneration.
- CLAUDE.md's claim that ops files are "committed" is wrong; fix the wording if it matters.

## Phase 2 — 68030 mode + LC II ROM (the big lever)

Goal (revised 2026-10-20): a colour-capable 68030 Mac (V8 family) that boots System
7.5.3. The original "fewer cycles per instruction → >100%" premise was wrong (see "ROM
choice reassessment"). Assets: `ROM/LC-II.ROM` (512KB, complete, checksum $35C28F5F)
and `ROM/Color-Classic.ROM` (1MB, $ECD99DC0). **Both are 68030 machines.** The earlier
"Color Classic = 68EC000" note was wrong.

Steps:
1. `main/CMakeLists.txt:5`: `set(EMBED_FILES "${ROM_DIR}/LC-II.ROM")`.
2. `m68kconf.h`: `M68K_EMULATE_030 OPT_ON`; regenerate ops as in Phase 1.
3. `mac_emulator.c`: `M68K_CPU_TYPE_68030`.
4. Re-derive EVERY address-pinned intercept against the new ROM before flashing. The
   current set is pinned to Classic ROM addresses and will silently misfire:
   F600/F601/F633–F642 boot markers, $41A44A SCSI counters, $4357F4 SEC-RD,
   $40315C/$4031BE/$4031CC/$4031E0/$4031EE DM traps, $08446C ADB flag, F2000 post-boot,
   $400768 UNIMP, ROM patches at $1C68/$0086/$011E. Use `tools/dis68k.py` +
   `tools/dump_rov_ptch_boot.py` against LC-II.ROM to find equivalents; some bypasses
   may be unnecessary on the LC II boot path — prove each one, don't assume.
5. Boot floppy: `OS_750_boot.dsk` (System 7.5.3) must be valid for LC II — verify
   against the ROM's own OS-version check before assuming.

Done when: Finder boots on LC II ROM, all intercepts re-proven, SPEED > current.
Risks: this is the highest-risk step in the whole roadmap — a wrong intercept address
reproduces the known F5251+/SCSI-range crash. Keep the Phase-1 build in git as the
rollback point. Budget: 030 tables larger than 020; re-check `size` and PSRAM.

### Phase 2 attempt 1 (2026-10-16) — build switched, boot fails, WIP committed

Build switch (all committed):
- `main/CMakeLists.txt`: EMBED_FILES → LC-II.ROM (512KB, same $400000-$43FFFF map).
- `mac_emulator.c`: embed symbols → `_binary_LC_II_ROM_start/end`; CPU type → 68030.
- `m68kconf.h`: 020 AND 030 both OPT_ON (030 is a superset; the generated ops gate the
  040-FPU opcodes with a runtime `CPU_TYPE_IS_030_PLUS`, so enabling 030 makes the ops
  table reference `m68040_fpu_op0/1` → `m68kfpu.c` + `softfloat/softfloat.c` must link).
- `components/musashi/CMakeLists.txt`: added `softfloat/softfloat.c` to SRCS. Binary 2.46MB
  (+82KB vs 68000), 60% free in the 4MB app partition. No PSRAM-table change (fixed size).

Boot observation (Classic-pinned intercepts still in place, LC II ROM embedded):
- ROM self-test passes naturally (BOOT2-KEY: BoxFlag=0, MachByte=0 — the LC II ROM doesn't
  set the Classic's low-mem markers; UNIMP read at $400768 = $00000000, different code there).
- Boot block runs: BOOT3-F633..F642 fire with RAM PCs ($2F1C-$3A1A, SP $2600) — boot block +
  early System code executing. Boot crawls at 17-23% (memory test + SCSI countdowns NOT
  bypassed on this ROM).
- System-file load FAILS: at F2000 all low-mem globals zero (MemTop/BufPtr/ScrnBase/StkBase/
  HeapEnd), trap table zero=256, "12 critical Toolbox traps UNIMPLEMENTED", OS[$6A] HUnlock
  = $00000000 UNIMP. FinderName="" then CRASH F2007: PC=$C95BC9 (SWIM/IWM space) SP=$002604.
- No DM-READ/DM-CTL/DM-STAT/SEC-RD lines ever fired — the LC II ROM's Device Manager disk
  code lives elsewhere; the boot block's floppy reads diverge and PC jumps into SWIM space.

Next steps for a future session (the LC II ROM needs its OWN bypass set, derived by
disassembly against LC-II.ROM, same as Phase 2 step 4):
1. Find the LC II ROM's sector-read/Device Manager routines (the equivalents of $40315C/$4031BE/
   $4031CC/$4031E0/$4031EE and $4357F4) — hunt for the .SONY request/complete loop pattern with
   dis68k.py over LC-II.ROM; the boot block got far enough that the disk controller is the
   immediate blocker.
2. Memory-test + SCSI-countdown bypass equivalents (boot speed 17-23% shows they're inactive).
3. UNIMP handler + checksum/RAM-test patch equivalents (LC II ROM's $400768 area is not the
   UNIMP stub; find its illegal-instruction handler).
4. The emulator's OS-trap-table / Toolbox checks read Classic low-mem locations — verify they
   read OS-version-dependent (7.5.3) addresses, not ROM-dependent ones.

### Phase 2 attempt 2 (2026-10-19) — ROM identity: LC-II.ROM is a bad dump

> **SUPERSEDED 2026-10-20 — conclusions below are WRONG.** `4E FA xxxx` is
> `JMP d16(PC)`, and `4D FA 00 06 4E FA xxxx` is Apple's BSR6 idiom. The "far calls to
> $A80000-$AFFFFF" are short PC-relative jumps inside the ROM (@1C52→$1D00,
> @3076→$3124), so LC-II.ROM is complete. `$2A: 4E FA 00 60` is `JMP $8C(PC)`, not
> `JSR $600000`, so the $600000/$5FFFD2 Start-Manager-alias model is unfounded.
> See attempt 4.

Analysis only (no build). Findings, all verified against the ROM files with
`tools/romcheck.py`, `tools/scan_calls.py`, `tools/rom_dump.py`, `tools/dis_lc2_boot.py`:

- **LC-II.ROM (512KB) is a truncated dump of a 1MB ROM.** Its code contains genuine
  68020 far-call idioms (`LEA $+6,A6` then `JSR.L`) into $A80000-$AFFFFF — @1C52
  JSR $AC0000, @3076 JSR $AC6600, @4A358 JSR $A808AA, @47B26 JSR $AC117C. Those are
  offsets $808AA-$C6600, beyond the 512KB file. The file's checksum ($35C28F5F) is
  self-consistent, but that only proves the dump tool recomputed it over the truncated
  image; it is not the known Mac LC ROM ($350EACF0) either. The real LC II ROM is 1MB.
- **V8 ROM aliasing proven cross-ROM:** LC-II calls $A44CDF, Color-Classic calls
  $644CDF, and LC-II[$44CDF] == CC[$44CDF] byte-identical → the V8 ROM is aliased at
  BOTH $600000-$6FFFFF and $A00000-$AFFFFF. Expert V8 map: ROM $40A00000-$40A7FFFF →
  $A00000-$A7FFFF 24-bit view (A24-A30 undecoded), overlay mirrors ROM at $0-$7FFFF,
  RAM $0-$9FFFFF. The compact-Mac "RAM moved to $600000" window is pre-V8 only.
- Boot entry (all three ROMs): reset PC=$2A overlay-relative (lands in the vector-$A
  slot). LC-II/CC bytes $2A: `4E FA 00 60 00 00` = JSR.L $600000 (6 bytes; dis68k.py
  misdecodes 4E FA as a 4-byte PC-rel JMP). $600000-$67FFFF is RAM on V8 — the Start
  Manager must be resident there at reset. ROM[$60000-$6FEFF] is FF00 ILLEGAL-guard
  padding; real code resumes at $6FF00 (Start Manager disk I/O touching boot globals
  $0134/$02FC/$0300/$0304/$038A/$038E). Only reference to $600000-$67FFFF in LC-II.ROM
  is the boot JSR itself.
- Color-Classic.ROM (1MB) is complete and self-consistent ($ECD99DC0), same boot
  structure. Caveats: 68EC000 (020-class, no FPU) and expects a color display.

Decision point: Phase 2 needs a genuine 1MB LC II ROM (re-dump) or switch ROM_NAME to
Color-Classic.ROM. Either way the memory map under MAC_ROM_LC_II must become the V8 map:
ROM aliased at $600000-$6FFFFF and $A00000-$AFFFFF (mask to ROM size), RAM $0-$9FFFFF,
overlay mirror $0-$7FFFF→ROM, drop the $600000→RAM[$0] compact window.

### Phase 2 attempt 3 (2026-10-19) — Color-Classic.ROM boots to F360+, MemTop never set

Build state (uncommitted, working tree): ROM_NAME=Color-Classic.ROM, MAC_ROM_LC_II=1,
V8 map in mac_memory.c (ROM $A00000-$AFFFFF + alias $600000-$60FFFF→rom[$6FF00+],
RAM $0-$9FFFFF, fast-RAM test `addr<0x580000 || (addr>=0x610000 && addr<0x800000)`).
Crash detector in mac_emulator.c re-derived for V8 (invalid PC = device windows only).

Observed (serial_watch.log):
- Reset PC=$2A SP=$ECD99DC0; JSR.L $600000 → alias → Start Manager runs (V8BOOT ov=1).
- PC walk $1002F8→$1FFAD0 stride +0x7D0 (BB trace) — ROM's continuation executing in
  RAM $100000-$1FFFFF.
- Crash F7: PC=$580252, SP=$0FF9F8; PCBYTES@180252 = all zeros — the RAM code at
  $1FFAD0 calls $580252 but nothing was ever written there.
- MemTop ($031C) NEVER written (V8MT trace silent) → F100/F400 report MemTop=0.
- V8WTEST/V8CPY write traces silent: nothing writes $1002F8/$1FFAD0/$360000 through
  m68k_write_memory_* or mem_write_byte — yet PC executes $1002F8. Either the walk
  region is written through a path not instrumented, or the +0x7D0 "walk" is not
  sequential execution.
- ROM poll sites `TST.B (A3); BPL self` all in file $70252-$70392 (alias $600340-$6003B8)
  — the ROM's I/O-wait loop; V8DEC trace at $600EBE/$600352 never fired → that loop
  never ran → the continuation is NOT decompressed, it is copied (or expected resident).

Open questions to resolve next:
1. Who writes $100000-$1FFFFF (the continuation copy)? Instrument the copy directly
   (trace writes to $1FFAC0-$1FFAF0 in mem_write_byte — added, awaiting run).
2. Is $580000-$5FFFFF RAM on V8 (SCSI driver code resident there) with the SCSI
   device window elsewhere? The ROM calling $580252 as code suggests yes — re-derive
   the V8 device map from the ROM's own abs.L references.
3. The boot-block read from the floppy (OS_750_boot.dsk) must complete for the Finder;
   DM intercepts are Classic-addressed ($40xxxx) and cannot match V8 ROM reads.

**Attempt 3 resolution (2026-10-19): the alias lower bound was the bug.** The ROM's
Start Manager stub begins at file $0FFD2 = bus $5FFFD2 (preceded by zeros at $0FFD1);
the alias is $5FFFD2-$610000 (file $0FFD2 + (addr-$5FFFD2)), NOT $600000-$610000.
With the stub served as RAM zeros it was a zero-walk; with the widened alias it
executes real ROM code, copies the continuation into RAM ~$394829, and the OS runs
from there: BOOT3-F6xx PCs $7Axxxx-$7Fxxxx, F4000 reached at 17% real-time, no crash.
The stub's pushed $27100000 (masked $100000) return is superseded by the copy's
real destination. MemTop($031C) still reads 0 in Classic-keyed diagnostics — V8
globals are A5=$002600-relative, so those reads hit the wrong page (noise). BADPC
filter still Classic-keyed (noise). Remaining: re-derive the disk-I/O completion
intercepts (BOOT_READ_HELPER/DM_READ/SPIN/CTL/STAT) against V8 ROM addresses when
the System-file load stalls; watch the run for the load phase.

**Attempt 3 cont'd (2026-10-20): V8 boot reaches Finder.** "FinderName=... CurApName=..."
at t=641s (10.7 min), no crash, V8SPIN never fired (the generic same-(addr,PC) byte-read
detector replaced the fixed-window V8DEC trace — the ROM's wait loop `4A 13 6A FC` lives
at file $70252+ = bus $CF2652+, i.e. it runs in copied RAM, so fixed bus windows cannot
catch it). Boot-phase speed 5% real-time = the V8 RAM-size test running legitimately.
Post-boot PC-HIST (V8 mapping fixed: `pc_hist[(pc>>12)&511]` — the first mapping
overflowed pc_hist[640] via V8 PCs $5FFFD2/$7Dxxxx): 85 samples, max share 4% → Finder
is NOT a compute spin; it plateaus at 15-19% real-time. Working theory: Finder is
disk-decode-bound through IWM/SWIM because the DM disk intercepts are keyed to
Classic/LC-II addresses, not Color-Classic ROM (and V8 OS code executes in copied RAM
at run-varying addresses, so address-keyed intercepts may need pattern-scan instead).
Instrument: `v8_iwm_reads` counter in mac_iwm.c's iwm_read, logged as IWMR per 300-frame
SPEED report — flashed, awaiting read. Millions of reads/300 frames confirms the theory;
near-zero means the 18% plateau has another cause.

**Attempt 3 cont'd (2026-10-20, evening batch): CPU mode was the plateau.** Setting
`m68k_set_cpu_type(M68K_CPU_TYPE_68000)` (Color Classic is a 68EC000 machine; Musashi's
030 core carries per-instruction overhead) took idle from 17% to 73% and boot from 636s
to 157s. Finder launches; the LCD stays blank (green = stale panel, our LUT only emits
black/white, draw_bitmap never errors). ScrnBase=$040060 is valid but its framebuffer is
never painted (xor constant, bar all zeros); 99% of post-Finder samples sit in 16KB page
$010000-$01FFFF (OS code from the boot floppy) with occasional PC=$000036 visits — the OS
runs but does not draw. DM intercepts re-verified: V8 ROM file $0BB24 == Classic file
$0315C byte-for-byte (except JMP disp) → the LC-II constants are correct for Color-Classic;
the `#if !MAC_ROM_LC_II` gating on the DM/BOOT_READ call sites was reverted (runs 7/8 were
identical with and without it). New probe at F3000: TheDisplay GDevice ($0CA6) → Port
baseAddress + colorMode — hypothesis is the V8 OS boots in COLOR mode and draws to an
8-bit buffer that is not ScrnBase. If so, the renderer must read that buffer (rowBytes)
or the OS must be forced to mono. PAGE dump base fixed to `<<14` to match the sampler.

### Phase 2 attempt 4 (2026-10-20) — host harness; three root causes; Egret

New tool: **host harness** `MacintoshESPEmulator/host/`. It compiles the unmodified
emulator core for x86 with ESP-IDF shims. 3000 frames run in 0.5–0.8 s, versus about
7 min on the board. `make TRACE=1` adds a per-instruction trace (`--trace N`, plus
`trace_ring.txt` at exit). Its logs reproduce the board's line for line. Debug V8 here
first; use the board only to confirm and to measure speed.

Root causes found by instruction trace (all fixed in the working tree):
1. **CPU type.** The V8 build ran Musashi as a 68000. The Color Classic/LC II reset code
   opens with `$F4AC` (cache op), which faults on a 68000; the Classic RECOVERY hack then
   jumped to $0036. Every 68000-mode "boot to Finder" (incl. "72% idle") ran on a
   corrupted path, which is where the `$2704` (saved SR) smeared over MemTop/ScrnBase
   came from. Fix: `M68K_CPU_TYPE_68030` under `MAC_ROM_LC_II`.
2. **Overlay rule.** The Classic "first VIA ORA write clears the overlay" latch fired at
   ROM $47EA and removed the ROM under the CPU (zero walk → $2000CC). V8 rule: the
   overlay clears on the first access to the real ROM window $A00000 (MAME-style).
   Fix: `via_clear_overlay()` called from `mem_read_byte`; ORA latch is Classic-only.
3. **Device map.** Taken from the ROM's own decoder table (CC file $3490; LC II has the
   same constants): VIA1 $F00000, SCC $F04000, SCSI $F10000, ASC $F14000, SWIM
   $F16000, VDAC $F24000, pseudo-VIA2/RBV $F26000, VRAM $F40000; $FC0000 is the diag-ROM
   probe. VIA1 and SWIM moved; VRAM/VDAC/pseudo-VIA2 still TODO (needed for video).
Also: both RECOVERY redirects are disabled for V8, so real faults surface.

Current blocker: **Egret handshake** spin at $AB39BA (CC) / $A14E20 (LC II). It is
`BTST #2,$1A00(A2)`, waiting for VIA1 IFR.SR, using PB3/PB4/PB5 handshake lines.
LC-II.ROM follows the identical path on the host (overlay clears at $A02E3E, then the
Egret spin). Egret HLE is in progress (host-first). After Egret: video (VRAM,
pseudo-VIA2 monitor sense, VDAC CLUT, depth-aware renderer, 512×384 → 480×320).

ESP32 config A/B (same firmware, steady idle, ms per 300 frames):
| Config | Idle | Verdict |
|---|---|---|
| MEMW + DIO (baseline) | 6936 | — |
| DUPLDST + DIO | 7023 (−1.3%) | rejected (also needed `-Wno-error=volatile` for IDF v6.1) |
| MEMW + **QIO** | **6860 (+1.1%)** | kept, in `sdkconfig.defaults` |

### Phase 2 attempt 5 (2026-10-20) — MMU stub, video found, SWITCHED TO LC II

- **PMMU stub** (`components/musashi/m68kmmu.h`): handles PMOVE TC/SRP/CRP/TT0/TT1/MMUSR,
  PLOAD/PFLUSH(A) as no-ops, and PTEST via a software table walk, with correct EA lengths.
  There is no per-access translation (`M68K_EMULATE_PMMU` stays OFF). The ROM's own MMU
  tables were verified to map the same addresses as our 24-bit masking.
- **Musashi bug fixed earlier in this session:** `CPU_TYPE_IS_010_LESS` was a constant 1
  with 020 emulation on. That made 020/030 decode indexed EAs and exception frames the
  68000 way, which is probably the real cause of the Phase 1 failure.
- **Egret/Cuda HLE** (`main/mac_egret.c`): the Color Classic is Cuda, LC/LC II are
  Egret. It handles PRAM, the RTC, IIC, autopoll and 6805 memory reads/writes, and the
  LC II's 32-byte streaming PRAM reads.
- **V8 RAM:** no aliasing above `ram_size` (reads $FF), so the ROM sizes 2MB correctly.
- **Machine ID** (VIA1 PA): CC = $0B, LC II = $5D (ProductInfo $A03BA6, BoxFlag $0D).
- **VIA IRQ** is now pushed to the CPU immediately on V8. A stale IRQ had zeroed
  TimeDBRA, causing a divide-by-zero Sad Mac.
- **SWIM** write-FIFO model for clock calibration. SCSI regs ($F10000-$F13FFF) and SCC
  ($F04000) are mapped for V8.
- **Video** (`main/mac_v8video.c`): 512KB VRAM at $F40000 (PSRAM), VDAC CLUT at $F24000,
  and pseudo-VIA2 at $F26000. Sense = 2 (12" RGB 512×384). The VBL is pulsed on
  SlotIFR bit 6. The ROM draws the grey desktop and the cursor into VRAM.
  ScrnBase=$50F40000, rowBytes=1024 (fixed). CC boots at 8bpp; LC II starts at 1bpp.
- **Build switched to LC-II.ROM** (`main/CMakeLists.txt` ROM_NAME; new `MAC_V8_CUDA`
  define: 1 = CC, 0 = LC II; host `make MAC_V8_CUDA=1` builds CC). The ESP32 binary went
  from 0x2F5350 to 0x273350 (**−520KB flash**, 38% free).
- **Next blocker (both ROMs):** floppy boot loop at $A01484-$A014CE. The V8 SWIM read
  path never returns a sector, because `mac_iwm.c` is tuned to Classic-ROM PCs and
  register 6 lacks set/clear semantics. Fix with correct SWIM ISM reads or a V8 .Sony
  read intercept. Also: `mac_video.c` still renders 1-bit main-RAM ScrnBase, so it needs
  a V8 path that reads VRAM through the CLUT. VRAM goes through the byte slow path and
  will need a 16/32-bit fast path.

### Phase 2 attempt 6 (2026-10-20) — LC II boots to Finder at native 480×320 (host)

- **Floppy:** `main/mac_v8disk.c` replaces only .Sony Prime. After the driver's Open,
  JPrime ($0226) is repointed per frame to a 12-byte stub served from unmapped bus
  $FE0000 (`TST.B $FE0100` runs `v8disk_prime()`, then JIODone). Reads come from the
  embedded image. Writes return wPrErr, and the volume mounts locked. Nothing runs per
  instruction.
- **SWIM on V8:** real IWM↔ISM mode switching, a 16-entry drive sense table, and drive 2
  reported not installed (this fixed "Disk initialization failed!").
- **VIA timers on V8** count at CPU/10 (E clock) with mid-slice interpolation. This fixed
  the System timing calibration's divide-by-zero and the Time Manager T2 spin.
- The Classic F600 boot-patch block is gated off for V8.
- **Renderer:** `v8_render()` in `mac_video.c` reads VRAM at 1/2/4/8/16 bpp through the
  CLUT (MAME index rule). It draws 1:1 at 480×320 and scales otherwise. The palette is
  byte-swapped for esp_lcd; panel colour order is UNVERIFIED on hardware.
- **VRAM fast path** for 8/16/32-bit accesses: about 13% less host CPU while drawing.
- **Native 480×320:** `v8video_patch_rom()` patches the 512×384 VPBlocks in the
  declaration ROM ($7EC18-$80000 on LC II; 13 blocks, 5KB PSRAM copy) and recomputes the
  Slot Manager CRC. The copy is served on byte reads; the flash image stays intact for
  the startup word-sum. Main device rect, pixel map bounds and CrsrPin all = 480×320.
- **PRAM $01 = 1** (Egret init): the SCSI startup-disk wait drops from 20 s to 1 s. The
  boot-block read moved from about F1550 to about F642.
- Host: LC II, Classic and Color Classic all reach Finder in 20000 frames with no
  CRASH. ESP32 binary 0x2718E0 (38% free). **First LC II board flash 2026-10-20.**

### Decision (user, 2026-10-20): LC II is the target ROM

Ship on **LC-II.ROM**. Color-Classic.ROM is a bring-up aid only. Switch `ROM_NAME` as soon
as LC II reaches the same boot stage, then drop Color Classic unless it proves to offer
something LC II can't. Known LC II gaps (host, `-DEG_CUDA=0`): it passes the Egret
exchanges but stalls in the RAM test (memory filled with $B6DB6DB6). Its machine ID
(VIA1 port A ID bits) and RAM layout (4MB on-board on a real LC II; we have 2MB) are not
modelled yet. The MMU stub, V8 map, overlay rule and video work are shared code.

### Video plan: native 480×320 mode (both ROMs)

LCD = ST7796 480×320 RGB565 over SPI at 40 MHz. Scaling a Mac mode is lossy: 512×384 →
0.94×0.83, which squashes the picture or drops 1 line in 6. 640×480 is unreadable. Both
ROMs carry monitor tables (searched for `0000 0000 hhhh wwww` rects). LC II has
512×384, 640×480, 640×870 and 560×384 at file $7F7CC-$7FEDE. CC has the same set plus
640×400, 512×342 and 1152×870 (around $9AC38 and $F6F66). The monitor is chosen by the
sense lines we emulate (pseudo-VIA2 $F26000), on either machine.
Plan:
1. Implement VRAM ($F40000), VDAC CLUT ($F24000) and the pseudo-VIA2 sense lines. Boot as
   a 512×384 colour monitor first, using the existing scaler with a depth-aware path.
2. Confirm which table entry the V8 video driver actually uses (bounds, rowBytes, depth
   modes). Then patch that entry at ROM load to **480×320** (the same mechanism as the
   Classic ROM patches; the ROM checksum check is already skipped).
3. The renderer becomes 1:1: a CLUT lookup per pixel, no scaling tables. That is
   crisper and cheaper; an 8-bit frame is 150KB vs 192KB at 512×384.
Risks: code that assumes 512-pixel rows, and the driver's own timing/rowBytes
arithmetic. Validate on the host with PNG screenshots before flashing.

### ROM choice reassessment (2026-10-20)

**Emulation speed is set by how many 68k instructions the host executes per second, not
by the ROM.** Real-time % only compares against the emulated clock we choose
(`MAC_CPU_HZ` = 7.8336 MHz for every build today). Musashi's 68030 cycle table charges
fewer cycles per instruction, so a fixed cycle budget runs *more* instructions per frame.
The real-time % therefore drops, while useful work per real second stays the same. What
actually changes the user-visible speed:

| Factor | Effect | Notes |
|---|---|---|
| CPU core mode (68000 vs 68020/030) | small–moderate | the 030 path has extra decode/EA work; measure on a correct V8 boot (the old "17% vs 91%" came from a broken boot) |
| Screen depth | **large** | QuickDraw moves 1/2/4/8× the bytes at 1/2/4/8-bit; the renderer reads the same multiple; 1-bit is cheapest |
| RAM size touched | small | PSRAM cache pressure |
| ROM routines vs disk patches | negligible | System 7.5.3 code dominates either way |

Candidate ROMs ranked for raw speed:
1. **Mac Classic (current, Classic.ROM)**: 68000, 1-bit 512×342 (21KB framebuffer,
   native to our renderer). The fastest possible target, and proven at 91% idle. Limits:
   4MB RAM max, mono only.
2. **Classic II**: 68030 but 1-bit 512×342 built-in, Egret, V8-family. Same speed as LC
   II at 1-bit. Its ROM is not on hand (it would need a dump), and it gains nothing over
   LC II set to 1-bit.
3. **LC II (LC-II.ROM)**: 68030, colour 512×384 at 1/2/4/8-bit (selectable in
   Monitors), 512KB ROM. Speed ≈ Classic II when the Mac is set to 1-bit; slower in
   colour by the depth factor.
4. **Color Classic**: same machine class as LC II, but the ROM is 1MB (~908KB used; the
   rest is a larger multi-machine driver set). No speed gain, and it costs 512KB of flash.
5. Mac LC (68020): no gain over 030 in Musashi with PMMU off. Mac II/Quadra (FPU/68040,
   bigger ROMs): slower.

**Decision:** no ROM is faster than Classic.ROM. Classic.ROM and LC-II.ROM are the
same size (512KB). At 1-bit the LC II does the same drawing work, and its only extra
cost is the 68030 core (unmeasured; the old "17% vs 91%" came from a broken boot). Once
V8 reaches Finder, compare both on wall-clock workloads. If LC II at 1-bit is within a
few % of Classic, **LC II becomes the single build** (1-bit for speed, colour on demand)
and Classic is retired to a fallback. For the colour/68030 track, use
**LC-II.ROM** (same speed as Color Classic, 512KB less flash, and the same V8 code path
under `MAC_ROM_LC_II`). Keep Color-Classic.ROM only until the V8 boot works, then flip
`ROM_NAME` in `main/CMakeLists.txt`. Run the colour build at 1-bit when speed matters.
Measure speed by wall-clock for fixed workloads (boot-to-Finder seconds, open-folder
time), not real-time %, when comparing machines.

## Phase 3 — Native intercepts for hot routines

Goal: replace the top ROM routines (from Phase 0 histogram — expect Line-A primitives,
BlockMove, memory manager, disk paths) with native C running on Mac RAM buffers directly.
Infrastructure already exists: `RES-TRAP` trampoline (log shows Line-A vector redirected
to $1FA500). Extend that pattern.

Steps:
1. For each routine, write a contract: inputs (D/A regs + Mac RAM it reads), outputs
   (regs + RAM it writes), and the condition codes it must fake. Musashi's CC state is
   not writable from a data-read intercept — decide per routine whether to fake CCs via
   a tiny 68000 epilogue in the trampoline or to avoid CC-dependent callers.
2. Implement handler in C in `mac_video.c`/new `mac_native.c`; operate on `ram[]` with
   big-endian macros; return through the trampoline.
3. Verify each routine by differential run: same scripted workload with intercept ON vs
   OFF, compare framebuffer checksums at fixed frame counts. Any divergence = contract
   bug; do not ship a routine without a passing diff.

Done when: top 3 routines pass differential verification and SPEED improves measurably.
Risks: subtle semantic divergence (rounding, clipping, flag states) that shows up only
in rare workloads. Differential testing is mandatory, not optional.

## Phase 4 — JIT (parked)

Translate 68000 basic blocks to Xtensa into an IRAM code cache (~300KB available).
Real 2–4× potential but large effort, and it will trip on every latent bug — including
the SCC-space PC anomaly from baseline. Only enter this phase after Phase 1's UNIMP
anomaly is zero and BADPC is zero.

## Definition of done (whole roadmap)

Finder runs on LC-II.ROM (colour, 68030) at a wall-clock feel comparable to the Classic
build at 1-bit, with all intercepts re-proven, or
Phase 3 alone reaches an acceptable feel and the user stops. Update this file's Status
line and tick results after each phase; log the actual SPEED numbers achieved.

## Ground rules for future sessions

- Build only via `cmd.exe //C "...\build.bat build|flash|monitor"` (v6.1 paths).
- Debug emulation logic on the host harness first (`MacintoshESPEmulator/host/`, see its
  README). Flash the board only to confirm a fix or to measure speed.
- Decode `4E FA` as `JMP d16(PC)`. Two whole attempts were lost to misreading it.
- Never raise `fast_slice` above 4000 (known deterministic crash).
- CrsrNew only on position change; event-queue posting for clicks (CLAUDE.md CRITICALs).
- One phase per session where possible; commit each phase separately so rollback is a
  `git revert`.
