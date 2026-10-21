# Plan: bare-metal Raspberry Pi 1 port (with a 3.5" SPI touch display)

Written 2026-10-21. Goal: run the same emulator core (Mac Classic and LC II)
on a **Raspberry Pi 1 without an operating system**, booting straight from the
SD card, using the user's **"TFT35" 3.5-inch SPI touch shield**. Status: not
started — this is the plan.

## Why it fits

- The emulator core is plain C and already runs unmodified outside the ESP32:
  the PC host harness (`MacintoshESPEmulator/host/`) is a single-threaded port
  with ESP-IDF replaced by small shims. A bare-metal Pi port is the same idea.
- The common "TFT35" shields are **480×320** — the exact resolution the
  emulator already renders natively (Classic and LC II 480×320 modes, game
  full view), so the display path carries over unchanged.
- The Pi 1 is much stronger for this workload: ARM1176JZF-S at 700 MHz,
  16 KB I + 16 KB D caches (+128 KB L2 shared with the GPU), 256/512 MB of
  fast SDRAM, no PSRAM bottleneck. Expect full real-time for both machines
  (estimate — to be measured).

## 1. What we need (to confirm before starting)

| Item | Why | Status |
|---|---|---|
| **Exact Pi model** (Pi 1 A, B, A+ or B+; RAM size) | B+/A+ have the 40-pin header the shield needs; RAM ≥ 256 MB is plenty | ask user |
| **Exact shield model** (e.g. Waveshare 3.5" RPi LCD (A)/(B), Kedei, "MHS-3.5"; photo of the PCB) | Selects the LCD controller (usually **ILI9486**, sometimes HX8357/ST7796), the SPI format (some boards use a 16-bit shift register in front of the controller) and the max SPI clock | ask user |
| **Touch type** | Most TFT35 shields use an **XPT2046 resistive** controller on SPI (CE1, IRQ on GPIO17); capacitive variants use I²C | ask user / identify on PCB |
| Shield pin mapping | Typical: SPI0 MOSI 10 / MISO 9 / SCLK 11, LCD CS = CE0 (GPIO8), touch CS = CE1 (GPIO7), LCD DC = GPIO24, LCD RESET = GPIO25, touch IRQ = GPIO17 — confirm from the vendor | verify |
| micro/SD card | FAT32 boot partition: firmware files, `kernel.img`, ROMs, disk images | user |
| Optional: USB keyboard + mouse (or hub) | Real keyboard input, which the ESP32 board can't do | user |
| Toolchain | `arm-none-eabi-gcc` (WSL: `sudo apt install gcc-arm-none-eabi`), GNU make | install |
| Bare-metal framework | **Circle** (github.com/rsta2/circle, GPL-3.0): SPI, GPIO, timers, interrupts, EMMC + FAT, USB HID, framebuffer. Licence note: linking Circle makes the Pi binary GPL-3.0; our MIT sources stay MIT | decide |

Alternative to Circle: a minimal from-scratch bare-metal runtime (start.S,
MMU/caches, SPI, EMMC/FAT via a small library such as FatFs). Smaller and
MIT-friendly, but USB keyboard/mouse support would be very hard without
Circle's USB stack.

## 2. How the Pi boots without an OS

1. The GPU ROM reads the SD card's FAT partition and loads **`bootcode.bin`**,
   then **`start.elf`** + **`fixup.dat`** (Raspberry Pi firmware files).
2. `start.elf` reads **`config.txt`** and loads **`kernel.img`** at `0x8000`.
3. The ARM core starts at `0x8000` in our code: set the stack, clear BSS,
   **enable MMU + caches** (essential for speed), init peripherals, run the
   emulator loop.

SD card layout:
```
/bootcode.bin  /start.elf  /fixup.dat   (Raspberry Pi firmware)
/config.txt                             (e.g. kernel=kernel.img, arm_freq)
/kernel.img                             (our emulator)
/mac/Classic.ROM  /mac/LC-II.ROM        (user-supplied)
/mac/custom_753.dsk  /mac/*.dsk         (user-supplied disk images, writable)
/mac/config.txt                         (which ROM + disk to boot)
```

## 3. Architecture of the port

New folder `ports/rpi1/` (nothing in `main/` changes, like the host harness):

| Piece | ESP32 today | Pi 1 bare metal |
|---|---|---|
| CPU emulation + machines | `main/*.c`, Musashi | **same files** |
| ESP-IDF APIs | real ESP-IDF | shims (reuse/adapt `host/shim/`: log → UART/HDMI console, heap_caps → malloc, esp_timer → BCM2835 system timer 1 MHz, IRAM/EXT_RAM attributes → empty) |
| Main loop / tasks | 2 FreeRTOS tasks (core 1 CPU, core 0 video + touch sampler) | one loop: run frame → render dirty strips → poll touch (single core; like the host harness, `TOUCH_SAMPLER_TASK=0`) |
| Display | `display_draw()` → ST7796 SPI DMA | `display_draw()` → **ILI9486 over SPI0** (DMA if Circle supports it); RGB565 (or the shield's required format) |
| Touch | FT5x06 capacitive, I²C, trackpad gestures | **XPT2046 resistive**, SPI CE1: read X/Y/pressure, filter, calibrate; feed `mac_touch.c` (trackpad mode as today, plus an optional **absolute** mode: finger = cursor position, natural for a resistive stylus) |
| ROM + disk | embedded in flash (`_binary_*` symbols) | **files on the SD card** loaded into RAM at boot (ROM) and accessed as files (disk) |
| Writes | PSRAM overlay, lost at power-off | write-back to the `.dsk` file on SD → **persistent saves** |
| Keyboard | none yet (planned ADB keyboard, plan step 6b) | **USB keyboard** via Circle's HID driver → ADB keyboard emulation (step 6b is shared work) |
| Clock | Egret/RTC tick, no real time | same; optionally set from a config value (no RTC on the Pi 1) |

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

- [ ] **R0. Identify the hardware**: Pi model, shield model/controller, touch
      type, pin mapping; set up `arm-none-eabi-gcc` + Circle (or decide on a
      from-scratch runtime).
- [ ] **R1. Hello, display**: Circle sample that initialises the ILI9486 over
      SPI and fills 480×320 test patterns; measure the achievable SPI clock.
- [ ] **R2. Touch**: XPT2046 reads, calibration (stored in `/mac/config.txt`),
      debug overlay of the touch point.
- [ ] **R3. Emulator core boots**: `ports/rpi1/` Makefile building `main/*.c`
      + Musashi with the shims; ROM and disk loaded from SD; render to the LCD;
      Classic boots to the Finder (compare screenshots/behaviour with the host
      harness).
- [ ] **R4. Input**: touch → `mac_touch.c` (trackpad + absolute modes);
      USB mouse; USB keyboard → ADB keyboard (shares plan step 6b).
- [ ] **R5. Storage**: writable disk images on SD (write-back, flush at Shut
      Down), disk/ROM selection via `/mac/config.txt`.
- [ ] **R6. LC II + colour**: LC II build on the Pi (colour panel needs the
      RGB565 palette path, already in `mac_video.c`), boot in 256 colours.
- [ ] **R7. Speed round**: measure boot-to-Finder and idle; tune (caches,
      `arm_freq`, SPI clock, DMA, switches).
- [ ] **R8. Docs**: `docs/learn/08-raspberry-pi-port.md` + build/flash guide.

## 6. Risks

- Shield variants differ (controller, SPI word format, max clock) — R1 settles it.
- Resistive touch needs calibration and filtering; a stylus works best.
- Circle's GPL-3.0 applies to the Pi binary if used (keep the port in its own
  folder with its own licence note).
- Single core: rendering and emulation share the CPU — keep dirty-strip
  rendering and use SPI DMA so the CPU isn't blocked on transfers.
