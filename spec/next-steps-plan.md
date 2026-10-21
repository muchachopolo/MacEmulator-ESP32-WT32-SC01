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
- [x] 1b. `M68K_INLINE_FETCH` OFF: −8% exec cycles (4560M → 4182M), Finder 246 → 227 s. Now OFF.
- [x] 1c. `MUSASHI_HOT_IRAM` ON: **−49%** (4560M → 2307M), Finder 246 → 172 s. Now ON (IRAM 75 KB used, 56 KB free with everything). Was: `MUSASHI_HOT_IRAM` (CMake var, `hot_iram.lf`, ~17 KB IRAM). Trim the list if IRAM
      overflows.
- [x] 1d. `MAC_MEM_CALLBACK_IRAM=1`: −9% (4162M), Finder 212 s. Now ON.
- [x] 1e. `M68K_OP_INDEX`: only −3% (4426M, run had touches), costs 128 KB PSRAM + 10 KB DRAM → left OFF; retest on top of hot IRAM later. Was: `M68K_OP_INDEX` (16-bit opcode index in PSRAM, handler/cycle tables in DRAM).
- [ ] 1f. `TOUCH_SAMPLER_TASK` 1 vs 0 (feel, and video fps while touching).
- [ ] 1g. Verify touch on the board: 1:1 tracking, no overshoot, slow nudge, tap, tap-then-drag,
      double-tap. LC II and Classic.
- [ ] 1h. Classic native build on the board: screen, touch, **sound and ADB timing with the new
      VIA timer model** (`CLASSIC_VIA_ECLOCK`), boot time, idle speed.
- [ ] 1i. Colour byte order on the panel (palette is byte-swapped for esp_lcd; panel uses BGR
      element order, untested in colour). Set the LC II to 256 colours in Monitors and check the
      Apple-menu/happy-Mac colours.
- [x] 1k. (board-verified 2026-10-21: LC II Finder 115 s, idle 100% real-time, 60-65% skipped; Classic idle 83% skipped) Idle fast-forward + throttle + V8 cycle scale (host-verified 2026-10-21, see CLAUDE.md
      "Performance Optimizations" 6-8). On the board: Finder idle should reach ~100% SPEED with
      `IDLE-FF:` ~60-85% skipped; check that the throttle holds 60 fps (no faster), touch/cursor
      feel, menu/drag, video fps. A/B `MAC_IDLE_FF=0` and `MAC_V8_CYCLE_SCALE=1`. Note that SPEED %
      and "exec cycles per 300 frames" change meaning with both (fewer CPU cycles per frame).
- [ ] 1j. **Decision:** compare wall-clock (boot-to-Finder seconds, open a folder, open a menu)
      for Classic vs LC II at 1-bit. If LC II is within ~20%, it is the single build. Otherwise
      Classic is the main build and LC II is the optional colour build. Record the numbers in
      `spec/performance-roadmap.md`.

**Board result 2026-10-21 (LC II, all winners + idle fast-forward + V8 cycle scale 2 + clock/IRQ
fixes):** boot-to-Finder **115 s** (was 246 s this morning); Finder idle **100% real-time** at the
60 fps throttle with 60-65% of CPU cycles skipped by fast-forward (from ~F9000, after the
Finder finishes its startup work). A/B method: `scratchpad/ab_round.sh`-style loop (build, flash,
watcher, Finder + 10 PROFILE lines, median), run from a clean `git worktree` so concurrent
agent edits can't leak in; abort on "Hash of data verified" missing.

## Step 2 — Input completeness (~half day)

- [ ] ADB keyboard through Egret (address 2, handler 2; Talk R0 key codes, R2 modifiers;
      autopoll like the mouse). In `adb_command` in `main/mac_egret.c`.
- [ ] On-screen virtual keyboard (touch overlay that toggles via a gesture or corner tap;
      drawn by our renderer, not by the Mac). Needed for typing filenames and dialogs.
- [ ] Classic: start the touch cursor from the real Mouse global, not (256,171) (the first touch
      currently jumps).
- [ ] PRAM persistence (Egret PRAM 256 B → NVS) so Mouse tracking, Monitors depth and the
      startup disk survive power-off.

## Step 2b — Bugs found 2026-10-21 (small)

- [x] LC II clock frozen (fixed 2026-10-21, host): the Egret/Cuda HLE now sends the MCU's
      unsolicited 1 Hz tick packet [03][00][00] (ROM unsolicited path: LC II $A14A88 ->
      $A14BE2, Cuda $AD026A -> Lvl1DT[0] = $A0B140 ADDQ.L #1,Time). Starts after
      SET_ONE_SECOND_MODE ($1B): Egret while autopoll is on, Cuda when mode != 0.
      Time now +1 per 60 frames on LC II and Color Classic (was frozen). Switch
      `EGRET_ONE_SEC_TICKS` (default 1).
- [x] Classic spurious interrupts (fixed 2026-10-21, host): `via_update_irq()` drives the
      level-1 line immediately on Classic too (`CLASSIC_VIA_IRQ_NOW`, default 1, in
      mac_emulator.h). Host: level-1 entries 14.0 -> 1.2 /frame (boot 9.8 -> 1.1); Finder
      idle 3744 -> 3629 instr/frame, 49.2k -> 45.7k cycles/frame. **A/B on the board.**

## Step 2c — Done 2026-10-21 (Classic polish + custom disk)

- [x] Classic boot screens centred on 480×320 (happy Mac icon immediates, welcome/DSAlert rect
      rewrite at $40155E; shutdown box follows). Remaining 512-constants: sad-Mac screen
      ($401092), disk-switch alert rect ($4044F8).
- [x] Classic RTC/PRAM chip (`main/mac_rtc.c`, `CLASSIC_RTC`): no AppleTalk alert (SPConfig $22),
      real clock, PRAM round-trips; idle FF now skips ~83% on Classic.
- [x] Classic Shut Down freeze fixed (async .Sony completion routine now called,
      `dm_async_complete`); Special→Restart = `esp_restart()` cold boot; MBState written as a
      whole byte ($00/$80) so the shutdown screen's Restart button works.
- [x] Custom boot disk: `tools/build_disk.py` (machfs; manifest, `--extra` apps, `--prep`
      desktop DB, `floppy` profile) → `ROM/custom_753.dsk` 2880K (flash limit ~2.89 MB;
      firmware 0x3DC6A0 of 0x3F0000). `DISK_NAME` build variable (default still OS_750_boot.dsk).
      Large volumes mount through the existing floppy paths (Classic: csCode 6 format list,
      drive writeProt $80 → read-only). The user's own image is built locally and never
      documented or committed.
- [ ] Open: games want 512×342 and a keyboard (see Step 2); writable disk (PSRAM overlay or
      flash); LC II Monitors cdev says "cannot be used" (V8 video not recognised).

## Step 2d — Done 2026-10-21 (later)

- [x] Writable disk Level 1: PSRAM copy-on-write overlay (`main/mac_disk_overlay.c`,
      `DISK_WRITABLE`, `DISK_OVERLAY_KB` 384 Classic / 256 V8), Classic `.Sony _Write`
      intercept ($4031CC, `read16_dm_write`), unlocked volume; lost at power-off. Level 2
      (separate flash partition + flush + NVS PRAM) still open.
- [x] Classic clicks: ROM-faithful PostEvent mirror (`CLASSIC_MOUSE_POSTEVENT`), no more
      $C0/$D8 elements; touch **game mode** (`TOUCH_GAME_MODE`): windowless front app →
      clicks posted at the menu bar so the Process Manager doesn't switch games out.
- [x] Native speed-ups (step 3b): `components/musashi/mac_native.c` — native A-line trap
      dispatcher (`MAC_NATIVE_TRAPDISP`, byte-identical), native dcmp 3 decompressor
      (`MAC_NATIVE_DCMP`, output-exact, 2239 checked calls + offline test of every compressed
      resource), ROM loop accelerator (`MAC_NATIVE_LOOPS`). Host: −81% (LC II) / −75% (Classic)
      instructions to Finder. **Board (Classic + custom disk): Finder at ~63 s (was ~105 s).**
- [ ] Follow-up: boot workarounds keyed to frame numbers (e.g. post-boot lean fetch at F2000)
      now trigger after the Finder is up; switch them to "Finder launched" detection to reach
      the estimated 20-30 s.
- [ ] Not done from 3b: resource-map search and LC II handle traps (no zero-cost hook point).

## Step 3 — Native acceleration (the "native OS" track)

Principle: do not rewrite Mac OS. Replace the hottest pieces with native C, guided by
measurement, and verify every replacement byte-for-byte on the host harness.

- [x] 3a. **Profiling (done 2026-10-21, host).** `host/host_prof.c` (`make TRACE=1`,
      `--profile-from/--profile-to/--profile-out`, `--vclock`); reports in `host/prof_results/`.
      Findings: Finder idle is ~90% inside _WaitNextEvent (Process Manager sleep loop:
      OSEventAvail + trap $ABF7 + PPC $A0DD, ~390 instr/pass, 54 passes/frame on LC II, 20 on
      Classic). Window drag is ~97% a 2-instruction Ticks wait (`CMP.L Ticks,D0 / BHI`; LC II
      $A193E8, Classic $40C5C2). Menus: ~38% MenuSelect, GetResource lookups. Boot: resource
      load/decompress (Get1Resource 22-45% self). Drawing traps (CopyBits, FillRect, text,
      BlockMove) are each <0.5% → **not** worth native replacement yet. The trap dispatcher is
      15-17% of all instructions. **LC II runs ~2× instructions per frame vs Classic**
      (20.8k vs 10.2k) because 68030 cycle timings are ~half at the same 130,560 cycles/frame.
- [x] 3a′ items 1-2 done 2026-10-21 (idle fast-forward + V8 cycle scale). **Remaining from 3a:** (1) idle fast-forward: end the frame early when the
      Mac is provably waiting (null-event polling within one tick, Ticks wait loops), with a
      60 fps real-time throttle; (2) LC II per-frame cycle budget scaled to match the Classic's
      instruction rate per tick; (3) native A-line trap dispatcher; (4) native resource
      decompression and GetResource lookup; (5) handle traps (HLock/HUnlock/HGetState/
      HSetState). Items 1-2 in progress 2026-10-21 (agent, host-first).
- [ ] 3b. **Native trap replacements (Phase 3)**: per 3a, start with the trap dispatcher, dcmp, resource lookup and handle traps (drawing traps are cold). Original guess was BlockMove/BlockMoveData,
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
- [ ] (Lower priority now: a 2880K HFS volume already mounts through the floppy path on both builds — see Step 2c.) Virtual SCSI hard disk (NCR 5380 target: INQUIRY/READ/WRITE) in a flash partition or
      PSRAM: about 1.5 MB of flash is free after the LC II switch.
- [ ] Optional: original Mac LC ROM (68020, no MMU, `350EACF0`) as a fallback if the LC II's
      MMU stub ever proves insufficient. ROM collection reference:
      https://github.com/macmade/Macintosh-ROMs (Apple copyright; user's call).

## Step 5 — Publish for other fans (repo is public)

Repo state found 2026-10-21: no top-level README.md and no LICENSE; vendor datasheet PDFs were
committed (ST7796s.pdf, esp32-wrover-b_datasheet_en.pdf, WT32-SC01 datasheet, Getting Started
guide; since removed, links in `docs/hardware.md`); `origin/main` is at fbb47f5, so none of the recent work is on main; `ROM/` is correctly
gitignored.

Prepare the repo:
- [x] Merge `phase2-lc2-native` into `main` (fast-forwarded to dd3fd66 on 2026-10-21; repeat for later work).
- [ ] Write `README.md`: what it is plus a photo/GIF of the Finder on the board (the user
      provides it); exact board (WT32-SC01 v3.4) and where to buy; build and flash steps
      (`build.bat`, ESP-IDF v6.1); touch gestures; current speed and what works; known
      limitations; link to `spec/next-steps-plan.md` and the host harness README.
- [ ] ROM notice: users must supply their own `Classic.ROM` or `LC-II.ROM` and a System 7.5.3
      boot disk, and the README says where they go. **Never publish prebuilt firmware**
      (it embeds the Apple ROM and the disk image); releases are source-only.
- [x] Licence: MIT (2026-10-21): `LICENSE`, `THIRD_PARTY_NOTICES.md`. Keep Musashi's licence notice; credit Musashi,
      MAME (V8/Egret references) and Mini vMac (screen-hack precedent).
- [x] Replace the committed datasheet PDFs with links to the vendors' pages (PDFs removed; links in `docs/hardware.md`).
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

## Step 6 — External hardware: SD card, keyboard, joystick, Bluetooth, Wi-Fi (planned 2026-10-21)

Goal: more disk space and persistent saves, real input devices, and room in flash
for the Bluetooth/Wi-Fi stacks. Prerequisite from the user: **the expansion
header pinout** (photo or datasheet table) and the exact modules (SD module or
board revision with a slot; keyboard model — "SPI keyboard", possibly I²C like
CardKB/BBQ10; joystick type — analog, digital or I²C).

Facts to keep in mind:
- The ESP32 boots its firmware only from the internal 4 MB flash; the **Mac** can
  boot from a disk image stored on SD (the firmware stays in flash).
- The WT32-SC01 v3.4 (ESP32) most likely has **no SD slot** (the ESP32-S3
  "WT32-SC01 Plus" has one) — verify on the board. A micro-SD module in SPI mode
  can share the LCD bus (MOSI 13, CLK 14) and needs **2 extra GPIOs** (MISO, CS).
  SDMMC native mode conflicts with the LCD pins (14/15) — use SPI.
- The board's USB-C goes to a CH340 USB-UART; the ESP32 has no USB host, so USB
  keyboards need an adapter (CH9350L USB→UART module, 1 GPIO) or Bluetooth.
- I²C devices (keyboard/joystick modules) can share the touch bus (SDA 18, SCL 19,
  touch at 0x38) — no extra pins.

Steps:
- [ ] 6a. **SD card storage** (biggest win):
      - FAT-formatted card; mount with ESP-IDF `esp_vfs_fat_sdspi_mount` on the
        shared SPI host (coordinate with the esp_lcd SPI device; separate CS).
      - Disk images as files (`/sd/*.dsk`); the .Sony paths (Classic DM
        intercepts, `mac_v8disk.c`) and `mac_disk_overlay.c` read blocks from the
        file instead of the embedded flash image (keep a small PSRAM block cache;
        SPI SD ~1-3 MB/s, ms latency per block).
      - **Persistent writes** to the SD file (write-through or write-back with
        flush at Shut Down/periodic; replaces writable Level 2 in flash).
      - Optional: ROMs from SD too; boot-time selection (config file on the card,
        or touch menu) of ROM + disk; keep the embedded floppy as fallback.
      - Larger volumes: the floppy path already mounts 2880K; test bigger HFS
        volumes through .Sony; if the System insists on floppy limits, implement
        the SCSI hard-disk target (Step 4) on top of the SD file.
      - Flash then holds only code (~1.1 MB): ~2.9 MB freed (repartition: smaller
        app partition + optional storage partition).
- [ ] 6b. **Keyboard emulation core** (needed by every input option): ADB
      keyboard in the Egret HLE (LC II, address 2) and in the Classic ADB path;
      Mac key codes + modifiers; a test input over the USB serial console.
- [ ] 6c. **Physical keyboard + joystick** on the expansion header (or I²C bus):
      driver per module feeding 6b; joystick mappable to arrow keys + fire
      (keyboard games) or to mouse movement + button (mouse games).
- [ ] 6d. **Bluetooth** (needs 6a's flash space): BLE/Classic HID host for
      wireless keyboard, mouse and gamepads (no pins). Budget: internal RAM for the
      controller (~50-100 KB), some IRAM (53 KB free today); enable only when used;
      measure emulator slowdown (shared 32 KB cache) on the board.
- [ ] 6e. **Wi-Fi** (needs 6a): web page to upload `.dsk`/apps to the SD card and
      choose the boot disk; NTP clock for the Mac RTC; Wi-Fi buffers in PSRAM
      (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` already set). Off by default.
- Order: 6a → 6b → 6c and/or 6d → 6e. Pin budget decides 6c vs 6d for input.

## Ground rules

- Debug logic on the host harness first; flash only to confirm or to measure.
- Speed is measured as ESP32 exec cycles per 300 frames or wall-clock per task, never real-time %
  across different machines.
- Decode `4E FA` as `JMP d16(PC)` (two attempts were lost to misreading it).
- Agents: give each its own host build/scratch dirs and file ownership. Only the main session
  flashes the board.
