# Plan: bare-metal Raspberry Pi port (Pi 2 Model B + AC-TFT35 3.5" touch display)

Written 2026-10-21. Goal: run the same emulator core (Mac Classic and LC II)
on a **Raspberry Pi without an operating system**, booting straight from the
SD card, with a 3.5-inch SPI touch shield. Status (2026-10-21): R0–R3 and R5 done — the Mac Classic boots to the Finder on the Pi 2 with touch, from SD, saving to SD; R6 (LC II) and R7 (core 1 renders) built, under test; R4 (USB input) next.

**User's hardware (2026-10-21):**
- **Raspberry Pi 2 Model B v1.1 (2014)** — BCM2836, **4× ARM Cortex-A7 @ 900 MHz**
  (ARMv7, NEON/VFPv4), **1 GB RAM**, 40-pin header. (Better than the Pi 1 the plan
  first assumed: multicore lets us split emulation and display/touch like on the ESP32.)
- **AC-TFT35-V2.1** 3.5" shield — **identified (2026-10-21)** from the chips on the back
  (2× 74HC4094D, 74HC4040D, 74HC04D, XPT2046) as the classic **"3.5inch RPi LCD (A)"**
  design (Waveshare (A) and clones; Linux overlay `waveshare35a` / `tft35a`):
  **ILI9486** (chip-on-glass) driven in 16-bit parallel mode through two 74HC4094
  serial-to-parallel shift registers; the 74HC4040 counts SPI clocks and strobes WR
  every 16 bits. Consequences: **every transfer is a 16-bit word** (8-bit commands are
  sent padded to 16 bits; pixels are RGB565 words), the bus is **write-only** (no ID
  readback), DC/RESET are GPIOs. Pins: SPI0 MOSI GPIO10, SCLK GPIO11, LCD CS = CE0
  (GPIO8), **DC = GPIO24, RESET = GPIO25**; touch **XPT2046** on CE1 (GPIO7), MISO
  GPIO9, **PENIRQ = GPIO17**. SPI clock: Linux default ~16 MHz; many units run
  32 MHz+ — measure in R1.

Pi 2 specifics used below: the GPU firmware loads **`kernel7.img`** (ARMv7) at
`0x8000`; peripherals at `0x3F000000` (BCM2836); compile with
`-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard`; Circle builds with `RASPPI=2`;
secondary cores are started through the BCM2836 local mailboxes.

## Why it fits

- The emulator core is plain C and already runs unmodified outside the ESP32:
  the PC host harness (`MacintoshESPEmulator/host/`) is a single-threaded port
  with ESP-IDF replaced by small shims. A bare-metal Pi port is the same idea.
- The common "TFT35" shields are **480×320** — the exact resolution the
  emulator already renders natively (Classic and LC II 480×320 modes, game
  full view), so the display path carries over unchanged.
- The Pi 2 is much stronger for this workload: 4× Cortex-A7 at 900 MHz,
  32 KB I + 32 KB D L1 caches per core + 512 KB shared L2, 1 GB of fast
  SDRAM, no PSRAM bottleneck. Expect full real-time for both machines
  (estimate — to be measured).

## 1. What we need (to confirm before starting)

| Item | Why | Status |
|---|---|---|
| **Pi model** | Pi 2 Model B v1.1 — quad Cortex-A7 900 MHz, 1 GB, 40-pin header | **known** |
| **Shield model** | AC-TFT35-V2.1 = "3.5inch RPi LCD (A)" design: ILI9486 via 16-bit shift registers + XPT2046 (see above) | **identified**; max SPI clock to measure |
| **Touch type** | Most TFT35 shields use an **XPT2046 resistive** controller on SPI (CE1, IRQ on GPIO17); capacitive variants use I²C | ask user / identify on PCB |
| Shield pin mapping | Typical: SPI0 MOSI 10 / MISO 9 / SCLK 11, LCD CS = CE0 (GPIO8), touch CS = CE1 (GPIO7), LCD DC = GPIO24, LCD RESET = GPIO25, touch IRQ = GPIO17 — confirm from the vendor | verify |
| micro/SD card | FAT32 boot partition: firmware files, `kernel7.img`, ROMs, disk images | user |
| Optional: USB keyboard + mouse (or hub) | Real keyboard input, which the ESP32 board can't do | user |
| Toolchain | `arm-none-eabi-gcc` (WSL: `sudo apt install gcc-arm-none-eabi`), GNU make | install |
| Bare-metal framework | **Circle** (github.com/rsta2/circle, GPL-3.0): SPI, GPIO, timers, interrupts, EMMC + FAT, USB HID, framebuffer. Licence note: linking Circle makes the Pi binary GPL-3.0; our MIT sources stay MIT | decide |

Alternative to Circle: a minimal from-scratch bare-metal runtime (start.S,
MMU/caches, SPI, EMMC/FAT via a small library such as FatFs). Smaller and
MIT-friendly, but USB keyboard/mouse support would be very hard without
Circle's USB stack.

## 2. How the Pi boots without an OS

(Diagrams: [docs/learn/08-boot-explained.md](../docs/learn/08-boot-explained.md), part 3.)

1. The GPU ROM reads the SD card's FAT partition and loads **`bootcode.bin`**,
   then **`start.elf`** + **`fixup.dat`** (Raspberry Pi firmware files).
2. `start.elf` reads **`config.txt`** and loads **`kernel7.img`** (Pi 2; `kernel.img` on a Pi 1) at `0x8000`.
3. The ARM core starts at `0x8000` in our code: set the stack, clear BSS,
   **enable MMU + caches** (essential for speed), init peripherals, run the
   emulator loop.

SD card layout:
```
/bootcode.bin  /start.elf  /fixup.dat   (Raspberry Pi firmware)
/config.txt                             (e.g. kernel=kernel7.img, arm_freq)
/kernel7.img                            (our emulator)
/mac/Classic.ROM  /mac/LC-II.ROM        (user-supplied)
/mac/custom_753.dsk  /mac/*.dsk         (user-supplied disk images, writable)
/mac/config.txt                         (which ROM + disk to boot)
```

## 3. Architecture of the port

New folder `ports/rpi1/` (nothing in `main/` changes, like the host harness):

| Piece | ESP32 today | Pi 2 bare metal |
|---|---|---|
| CPU emulation + machines | `main/*.c`, Musashi | **same files** |
| ESP-IDF APIs | real ESP-IDF | shims (reuse/adapt `host/shim/`: log → UART/HDMI console, heap_caps → malloc, esp_timer → BCM2835 system timer 1 MHz, IRAM/EXT_RAM attributes → empty) |
| Main loop / tasks | 2 FreeRTOS tasks (core 1 CPU, core 0 video + touch sampler) | Pi 2: **core 0 = 68k emulation**, **core 1 = render + SPI transfer to the LCD**, core 2 = touch/USB polling (Circle multicore); a single-loop fallback like the host harness (`TOUCH_SAMPLER_TASK=0`) for bring-up |
| Display | `display_draw()` → ST7796 SPI DMA | `display_draw()` → **ILI9486 over SPI0** (DMA if Circle supports it); RGB565 (or the shield's required format) |
| Touch | FT5x06 capacitive, I²C, trackpad gestures | **XPT2046 resistive**, SPI CE1: read X/Y/pressure, filter, calibrate; feed `mac_touch.c` (trackpad mode as today, plus an optional **absolute** mode: finger = cursor position, natural for a resistive stylus) |
| ROM + disk | embedded in flash (`_binary_*` symbols) | **files on the SD card** loaded into RAM at boot (ROM) and accessed as files (disk) |
| Writes | PSRAM overlay, lost at power-off | write-back to the `.dsk` file on SD → **persistent saves** |
| Keyboard | none yet (planned ADB keyboard, plan step 6b) | **USB keyboard** via Circle's HID driver → ADB keyboard emulation (step 6b is shared work) |
| Clock | Egret/RTC tick, no real time | same; optionally set from a config value (no RTC on the Pi 2) |

ESP32-specific performance tricks (IRAM placement, PSRAM workarounds,
`MUSASHI_HOT_IRAM`, the inline-RAM switch) don't apply; the portable speed-ups
(native trap dispatcher, dcmp, ROM loops, idle fast-forward + 60 fps
throttle) carry over. Re-run a speed round on the Pi (the switches exist).

## 4. Display performance budget

480×320×16 bit = 307 KB per full frame. At an SPI clock of 16–32 MHz
(shield-dependent) a full redraw takes ~80–150 ms (≈7–12 fps), similar to the
ESP32 board (~61 ms at 40 MHz). The existing **dirty-strip skipping**
(`MAC_VIDEO_DIRTY`) keeps typical updates small (cursor, menus), so the Mac
should still feel responsive; DMA lets rendering overlap the transfer.
Higher SPI clocks are often possible on these shields — measure.

## 5. Phases

- [x] **R0. Identify the hardware**: Pi model, shield model/controller, touch
      type, pin mapping; set up `arm-none-eabi-gcc` + Circle (or decide on a
      from-scratch runtime).
- [x] **R1. Hello, display** (ILI9486 through the 16-bit shift registers: send the
      fbtft `fb_ili9486`/`waveshare35a` init sequence as 16-bit words, DC on GPIO24,
      RESET on GPIO25; find the highest stable SPI clock, 16 → 32 → 48 MHz): Circle sample that initialises the ILI9486 over
      SPI and fills 480×320 test patterns; measure the achievable SPI clock.
      **Result (2026-10-21, `ports/rpi/r1-display/`, Circle):** init sequence,
      orientation (MADCTL 0x28) and RGB colour order correct. **15.6 MHz clean,
      6.3 fps full frame** (= the wire limit). 31.2/41.7/62.5 MHz corrupt (bits lost
      in the 74HC4094 chain) and plateau at ~11 fps (Circle polled SPI write).
      R1b/R1c: **17.9 MHz is the limit (7.2 fps full frame)**. Above it the
      board drops pixels (a colour ramp breaks into shifted segments) and, with
      commands at the same clock, window commands too; sending commands at
      15.6 MHz fixes placement but not the dropped pixels. Driver:
      `ports/rpi/common/ili9486.*`, `CILI9486::MaxClock`. A 20-line strip costs
      ~8.6 ms, so dirty-strip rendering keeps the cursor and menus responsive;
      smaller dirty rectangles are an R7 option.
- [x] **R2. Touch**: XPT2046 reads, calibration (stored in `/mac/config.txt`),
      debug overlay of the touch point. Test app `ports/rpi/r2-touch/` (driver
      `ports/rpi/common/xpt2046.*`: 1.95 MHz, median of 7, pressure Z): 4-target
      calibration (axis swap + linear), then drawing with raw X/Y/Z shown.
      **Result:** works; swap 1, raw left 3619 / right 472 / top 730 / bottom
      3443 at the 40-px-inset targets → `XPT2046_CAL_*` defaults.
- [x] **R3. Emulator core boots**: `ports/rpi1/` Makefile building `main/*.c`
      + Musashi with the shims; ROM and disk loaded from SD; render to the LCD;
      Classic boots to the Finder (compare screenshots/behaviour with the host
      harness). **Done (2026-10-21, `ports/rpi/macemu/`, then named r3-emu):** single core, core
      compiled with `HOST_BUILD=1` + `MAC_FRAME_THROTTLE=1`, newlib libc + stubs,
      ROM and disk embedded in `kernel7.img` (Circle `KERNEL_MAX_SIZE=16 MB`).
      Boots to the Finder; touch moves the cursor and clicks once the desktop is
      up (touch is ignored until WMgrPort is valid, as on the ESP32). Boot time
      not yet measured.
- [ ] **R4. Input**: touch → `mac_touch.c` (trackpad + absolute modes);
      USB mouse; USB keyboard → ADB keyboard (shares plan step 6b).
- [x] **R5. Storage**: writable disk images on SD (write-back, flush at Shut
      Down), disk/ROM selection via `/mac/config.txt`. **Done:**
      `MAC_EXTERNAL_IMAGES`, `/mac/config.txt disk=`, write-back of changed
      blocks (1 s after the last write, at most every 10 s, before restart);
      confirmed on the Pi (a new folder survives power-off).
- [ ] **R6. LC II + colour**: LC II build on the Pi (colour panel needs the
      RGB565 palette path, already in `mac_video.c`), boot in 256 colours.
      Built: `kernel7-lc2.img` (`make MAC_ROM_LC_II=1`), chosen with `kernel=`
      in the SD card's `config.txt`; awaiting the first run.
- [ ] **R7. Speed round**: measure boot-to-Finder and idle; tune (caches,
      `arm_freq`, SPI clock, DMA, switches). Started: core 1 renders and samples
      touch (`videocore.cpp`, Circle `ARM_ALLOW_MULTI_CORE`), so LCD transfers
      no longer stall the 68k.
- [ ] **R8. Docs**: `docs/learn/08-raspberry-pi-port.md` + build/flash guide.

## 6. Risks

- Shield variants differ (controller, SPI word format, max clock) — R1 settles it.
- Resistive touch needs calibration and filtering; a stylus works best.
- Circle's GPL-3.0 applies to the Pi binary if used (keep the port in its own
  folder with its own licence note).
- Single core: rendering and emulation share the CPU — keep dirty-strip
  rendering and use SPI DMA so the CPU isn't blocked on transfers.
