# Performance roadmap — Macintosh ESP32 emulator

Spec written 2026-10-16. Purpose: durable plan so a future session can resume without
re-deriving context. Update the Status line and tick boxes as work lands.

Status: Phase 0 done (2026-10-16). Phase 1 attempted and rolled back (2026-10-16) — 68020
mode crashes the Classic-ROM boot; root cause recorded in Phase 1 below. Phase 2 IN PROGRESS
(2026-10-16): build switched to 68030 + LC II ROM, boots to the boot block but the System-file
load fails and it crashes at F2007 in SWIM space — LC II ROM needs its own bypass set; state
recorded in Phase 2 below. NOTE: working tree + flashed binary are the Phase-2 (broken) config;
the last fully working build is at commit c9abaef (68000 + Classic).

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
2. 68030 mode + LC II ROM (bigger win, high risk — see Phase 2 risks).
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

Goal: OS compiled for 68030 uses 030 addressing modes → fewer cycles per instruction.
Target: >100% real-time. Assets already present: `ROM/LC-II.ROM` (68030),
`ROM/Color-Classic.ROM` (68EC000 — 020-class, no FP; keep as fallback).

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

Finder runs at >100% real-time on the LC II ROM with all intercepts re-proven, or
Phase 3 alone reaches an acceptable feel and the user stops. Update this file's Status
line and tick results after each phase; log the actual SPEED numbers achieved.

## Ground rules for future sessions

- Build only via `cmd.exe //C "...\build.bat build|flash|monitor"` (v6.1 paths).
- Never raise `fast_slice` above 4000 (known deterministic crash).
- CrsrNew only on position change; event-queue posting for clicks (CLAUDE.md CRITICALs).
- One phase per session where possible; commit each phase separately so rollback is a
  `git revert`.
