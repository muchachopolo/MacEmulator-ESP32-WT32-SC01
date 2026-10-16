# Performance roadmap — Macintosh ESP32 emulator

Spec written 2026-10-16. Purpose: durable plan so a future session can resume without
re-deriving context. Update the Status line and tick boxes as work lands.

Status: Phase 0 done (2026-10-16) — results recorded below. Phase 1 not started.

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
