# Next-steps plan — Macintosh ESP32 emulator

Written 2026-10-21. Purpose: a durable, ordered implementation plan for a future session.
History and evidence live in `spec/performance-roadmap.md` (Phase 2 attempts 4–6, "ROM choice
reassessment", "Video plan"). Update the status checkboxes here as work lands.

## Current state (2026-10-21)

Committed and pushed 2026-10-21 on branch `phase2-lc2-native` (4 commits on top of fe6369d); not yet merged into `main`.

| Area | State |
|---|---|
| Build ROM | `LC-II.ROM` (`main/CMakeLists.txt` ROM_NAME; `MAC_V8_CUDA=0` → Egret). `Classic.ROM` = 68000 build (`ROM_NAME "Classic.ROM"` → `MAC_ROM_LC_II=0`). Color Classic = `MAC_V8_CUDA=1`, kept only as a reference. |
| LC II boot | Host: System 7.5.3 to Finder at native 480×320 1:1. Board: Finder at about t=203 s. |
| Classic build | Host: Finder at native 480×320 1:1 (`CLASSIC_NATIVE_LCD=1`), new VIA timer model (E-clock + mid-slice interpolation). **Not yet run on the board.** |
| Input | LC II: ADB mouse through Egret for the button; cursor position written directly (1:1, no acceleration). New `main/mac_touch.c` sampler task on core 0 at 60 Hz (`TOUCH_SAMPLER_TASK`). Tap-drag fix and dead-zone fix in both builds. Touch rework flashed 2026-10-21 (awaiting feel test). Keyboard: none. |
| Speed (host) | Classic −22%, LC II −31% (bus-error emulation off, `MAC_DIAG=0`, inline RAM/fetch, video dirty-skip). |
| Speed (board) | LC II Finder idle: `M68K_INLINE_RAM` ON = 3846M exec cycles per 300 frames (30%); **OFF = ~3200M (36–37%), so it is now OFF** (ESP32 I-cache; the PC showed the opposite). The earlier "2583M" baseline came from a different workload (the insert-disk dialog loop, before the slot-VBL cursor IRQ), so it is not comparable. Where idle cycles go is open → step 3a profiling. |
| Tools | Host harness `MacintoshESPEmulator/host/` (`make`, `make TRACE=1`, `--frames --shot-every --log --trace --stop-pc --stop-from --tap --swipe --watch --cpu`). |
| Flash | Binary about 0x27DE40, 37% of the app partition free. QIO flash on. |

Key files added this round: `main/mac_egret.c/.h` (Egret/Cuda HLE + ADB mouse),
`main/mac_v8video.c/.h` (VRAM $F40000, VDAC $F24000, pseudo-VIA2 $F26000, ROM VPBlock patch to
480×320), `main/mac_v8disk.c/.h` (.Sony Prime hook through a JPrime → $FE0000 stub),
`main/mac_touch.c/.h`, `components/musashi/m68kmmu.h` (PMMU instruction stub, no translation),
`components/musashi/hot_iram.lf`.

## Step 0 — Housekeeping (do first, ~30 min)

- [x] Review `git diff --stat` and commit in logical chunks (done 2026-10-21, branch `phase2-lc2-native`): (a) Musashi fixes
      (CPU_TYPE_IS_010_LESS, PMMU stub, bus-error off, inline paths), (b) V8 machine (map,
      overlay, Egret, video, disk, timers), (c) Classic native + VIA model, (d) touch, (e) host
      harness, (f) docs/spec. Do not commit `board_test/sdkconfig` noise, `tools/*_out.txt` or
      `E:PersonalMacintoshtoolsbuild_out.txt`.
- [x] `host/.gitignore` now tracks only sources; the scratch dirs were deleted (done 2026-10-21).
- [ ] Optionally remove the dead Start-Manager-alias leftovers and Classic-keyed V8 diagnostics
      (the FIRST-BAD-PC detector misfires on V8 ROM addresses).

## Step 1 — Board round: performance A/B and verification (~2 h, board required)

Method: flash, let it reach Finder idle untouched, then read 5+ steady `SPEED`/`PROFILE` lines.
Compare **exec cycles per 300 frames** (lower is better) and the boot-to-Finder time
(`App="Finder"` timestamp). Start `tools/serial_watch.py` right after flashing. Kill stale
Windows `serial_watch` python processes before flashing (they hold the COM port).

- [x] 1a. `M68K_INLINE_RAM OPT_OFF` (done 2026-10-21): about 16% fewer exec cycles at LC II
      Finder idle (3846M → ~3200M per 300 frames). Keep OFF on the ESP32.
- [ ] 1b. `M68K_INLINE_FETCH` on vs off.
- [ ] 1c. `MUSASHI_HOT_IRAM` (CMake var, `hot_iram.lf`, ~17 KB IRAM). Trim the list if IRAM
      overflows.
- [ ] 1d. `MAC_MEM_CALLBACK_IRAM=1` (~2 KB IRAM).
- [ ] 1e. `M68K_OP_INDEX` (16-bit opcode index in PSRAM, handler/cycle tables in DRAM).
- [ ] 1f. `TOUCH_SAMPLER_TASK` 1 vs 0 (feel, and video fps while touching).
- [ ] 1g. Verify touch on the board: 1:1 tracking, no overshoot, slow nudge, tap, tap-then-drag,
      double-tap. LC II and Classic.
- [ ] 1h. Classic native build on the board: screen, touch, **sound and ADB timing with the new
      VIA timer model** (`CLASSIC_VIA_ECLOCK`), boot time, idle speed.
- [ ] 1i. Colour byte order on the panel (palette is byte-swapped for esp_lcd; panel uses BGR
      element order, untested in colour). Set the LC II to 256 colours in Monitors and check the
      Apple-menu/happy-Mac colours.
- [ ] 1j. **Decision:** compare wall-clock (boot-to-Finder seconds, open a folder, open a menu)
      for Classic vs LC II at 1-bit. If LC II is within ~20%, it is the single build. Otherwise
      Classic is the main build and LC II is the optional colour build. Record the numbers in
      `spec/performance-roadmap.md`.

## Step 2 — Input completeness (~half day)

- [ ] ADB keyboard through Egret (address 2, handler 2; Talk R0 key codes, R2 modifiers;
      autopoll like the mouse). In `adb_command` in `main/mac_egret.c`.
- [ ] On-screen virtual keyboard (touch overlay that toggles via a gesture or corner tap;
      drawn by our renderer, not by the Mac). Needed for typing filenames and dialogs.
- [ ] Classic: start the touch cursor from the real Mouse global, not (256,171) (the first touch
      currently jumps).
- [ ] PRAM persistence (Egret PRAM 256 B → NVS) so Mouse tracking, Monitors depth and the
      startup disk survive power-off.

## Step 3 — Native acceleration (the "native OS" track)

Principle: do not rewrite Mac OS. Replace the hottest pieces with native C, guided by
measurement, and verify every replacement byte-for-byte on the host harness.

- [ ] 3a. **Profiling (cheap, host).** Add a trap histogram (A-line dispatch: trap word →
      count and cycles until return) and a ROM/System-code routine histogram to the host harness.
      Workloads: boot, Finder idle, open a window, drag a window, menu, list view. Output: top 20
      traps/routines by cycle share, for both builds.
- [ ] 3b. **Native trap replacements (Phase 3)** for the top 3–5. Likely: BlockMove/BlockMoveData,
      CopyBits/bit blits, FillRect/PaintRect/region fills, memory manager (NewPtr/NewHandle/
      SetHandleSize), text drawing (StdText/DrawChar). Mechanism: intercept at the A-trap
      dispatcher or the RES-TRAP trampoline. A C handler works on `ram[]`/VRAM with big-endian
      macros and returns through a tiny 68k epilogue to set condition codes. Gate: the host run
      with the replacement ON vs OFF produces identical RAM dumps and screenshots at fixed frames
      for every workload. Each replacement gets its own switch.
- [ ] 3c. **Ahead-of-time translated blocks in flash (revised Phase 4).** The boot disk is
      read-only, so the System, Finder and ROM code is fixed. Use the host harness to record hot
      68k basic blocks (bytes + hash), translate them on the PC to Xtensa, and link them into a
      flash section (about 1.5 MB free; the ESP32 can execute from flash, not PSRAM, and IRAM
      has only about 70 KB free). At run time, check the block hash at dispatch and jump to the
      native code, else interpret. Needs a translator for flags, exceptions and A-traps,
      invalidation on writes to translated ranges, and a strict differential test against the
      interpreter on the host. High effort; only after 3b if speed is still insufficient.

## Step 4 — Rendering and memory (as needed)

- [ ] Double-buffer `line_buf` in `mac_video.c` (+19 KB DMA RAM) so render and SPI overlap.
- [ ] Consider a 640×480 → 480×320 option only if wanted (lossy); native 480×320 is the default.
- [ ] Virtual SCSI hard disk (NCR 5380 target: INQUIRY/READ/WRITE) in a flash partition or
      PSRAM: about 1.5 MB of flash is free after the LC II switch.
- [ ] Optional: original Mac LC ROM (68020, no MMU, `350EACF0`) as a fallback if the LC II's
      MMU stub ever proves insufficient. ROM collection reference:
      https://github.com/macmade/Macintosh-ROMs (Apple copyright; user's call).

## Step 5 — Publish for other fans (repo is public)

Repo state found 2026-10-21: no top-level README.md and no LICENSE; vendor datasheet PDFs are
committed (ST7796s.pdf, esp32-wrover-b_datasheet_en.pdf, WT32-SC01 datasheet, Getting Started
guide); `origin/main` is at fbb47f5, so none of the recent work is on main; `ROM/` is correctly
gitignored.

Prepare the repo:
- [ ] Merge `phase2-lc2-native` into `main` (PR or fast-forward) and push.
- [ ] Write `README.md`: what it is plus a photo/GIF of the Finder on the board (the user
      provides it); exact board (WT32-SC01 v3.4) and where to buy; build and flash steps
      (`build.bat`, ESP-IDF v6.1); touch gestures; current speed and what works; known
      limitations; link to `spec/next-steps-plan.md` and the host harness README.
- [ ] ROM notice: users must supply their own `Classic.ROM` or `LC-II.ROM` and a System 7.5.3
      boot disk, and the README says where they go. **Never publish prebuilt firmware**
      (it embeds the Apple ROM and the disk image); releases are source-only.
- [ ] Licence: MIT suggested (user to confirm). Keep Musashi's licence notice; credit Musashi,
      MAME (V8/Egret references) and Mini vMac (screen-hack precedent).
- [ ] Replace the committed datasheet PDFs with links to the vendors' pages.
- [ ] GitHub topics: esp32, macintosh, 68k, emulator, wt32-sc01, classic-mac, system7.
- [ ] Remove or neutralise `CLAUDE.md`'s machine-specific paths if they would confuse users
      (or keep it as contributor notes).

Where to share (suggested order: 68kMLA + r/VintageApple with a short video first, then
Hackaday once polished):
| Place | Why |
|---|---|
| 68kMLA forum (68kmla.org) | Main vintage Mac community; most interested audience |
| Tinker Different forum | Retro Apple hardware tinkerers |
| E-Maculation forum | Emulator enthusiasts (Mini vMac, Basilisk II); compatibility help |
| Reddit: r/VintageApple, r/esp32, r/retrobattlestations | Wide reach; short video works best |
| Hackaday.io project page + tip to Hackaday | They feature "classic computer on a microcontroller" builds |
| Show HN | Technical write-up (68030 on ESP32, native 480×320 ROM patch, host harness) |
| YouTube / Mastodon (e.g. oldbytes.space) | 1–2 min boot + touch demo video |

## Ground rules

- Debug logic on the host harness first; flash only to confirm or to measure.
- Speed is measured as ESP32 exec cycles per 300 frames or wall-clock per task, never real-time %
  across different machines.
- Decode `4E FA` as `JMP d16(PC)` (two attempts were lost to misreading it).
- Agents: give each its own host build/scratch dirs and file ownership. Only the main session
  flashes the board.
