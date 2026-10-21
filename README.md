# Macintosh emulator for the ESP32 (WT32-SC01)

A 68k Macintosh that runs System 7.5.3 on a €20 ESP32 touch-screen board.
The emulator boots a real Apple ROM, loads System 7.5.3 from a disk image
embedded in flash, and runs the Finder on the board's 3.5" 480×320 LCD, with
the capacitive touch panel acting as a trackpad.

<!-- Screenshots / photos: the author will add them under docs/images/, e.g.
     ![Finder on the WT32-SC01](docs/images/finder.jpg)
     ![LC II in colour](docs/images/lc2-colour.jpg)
     ![Boot GIF](docs/images/boot.gif) -->
> *Photos and a short boot video are coming (docs/images/).*

## Features

- **Two machines in one codebase**, selected at build time:
  - **Macintosh Classic**: 68000, 1-bit, `Classic.ROM`.
  - **Macintosh LC II**: 68030 V8 machine with colour video and the Egret
    microcontroller, `LC-II.ROM`. The Color Classic ROM (Cuda) also boots and
    is kept as a reference.
- **Native 480×320 screen** on both machines. The ROM is patched so the Mac
  itself uses the LCD's resolution, and every pixel maps 1:1 (no scaling blur).
  On the Classic, full-screen games that need 512×342 get a scaled full view
  automatically.
- **System 7.5.3 to the Finder** from a 1.44 MB floppy image or a custom
  2.88 MB disk built with `tools/build_disk.py` (Apple menu items, control
  panels, SimpleText, your own apps).
- **Touch trackpad**: swipe moves the cursor, tap clicks, tap-then-drag drags.
  A "game mode" keeps full-screen games in front when you tap.
- **Writable disk (session only)**: writes go to a copy-on-write overlay in
  PSRAM. Saving files works, but everything is lost at power-off.
- **Real clock and PRAM**: Classic RTC chip and the LC II's Egret clock/PRAM.
- **Speed work**: hot Musashi code in IRAM, native C trap dispatcher and
  resource decompressor, idle fast-forward with a 60 fps real-time throttle.
- **PC host harness**: the same emulator core builds for Linux/WSL, boots in
  about a second, writes PNG screenshots, and has an instruction profiler.
  See [`MacintoshESPEmulator/host/README.md`](MacintoshESPEmulator/host/README.md).

## Supported ROMs

You must supply your own ROM image (see [Legal](#legal)). The emulator
checks nothing but the file name; these are the dumps it was developed with
(checksum = the first 4 bytes of the ROM):

| Machine | File in `ROM/` | Size | Checksum | Status |
|---|---|---|---|---|
| Macintosh Classic | `Classic.ROM` | 512 KB | `A49F9914` | Works on the board: Finder, touch, clicks, games |
| Macintosh LC II | `LC-II.ROM` | 512 KB | `35C28F5F` | Works on the board (Finder, 1-bit). Colour on the panel not yet verified |
| Macintosh Color Classic | `Color-Classic.ROM` | 1 MB | `ECD99DC0` | Boots on the host; reference only (costs 512 KB more flash) |

Boot disk: a bootable **System 7.5.3** 1.44 MB floppy image (the default file
name is `OS_750_boot.dsk`).

## Hardware

**Board:** Wireless Tag **WT32-SC01 v3.4** (ESP32-WROVER-B module, 3.5" ST7796S
480×320 SPI LCD, FocalTech FT5x06 capacitive touch). No extra parts are needed.

| Part | Details |
|---|---|
| SoC | ESP32-D0WD rev 1.0, dual-core Xtensa LX6 at 240 MHz |
| Flash | 4 MB on the tested boards (the label says 16 MB), QIO 80 MHz |
| PSRAM | 8 MB, about 4 MB addressable |
| Display | ST7796S, 480×320 RGB565 over SPI at 40 MHz |
| Touch | FT5x06 on I2C, address 0x38 |

Datasheets (vendor documents; not stored in this repo):
- Wireless Tag: *WT32-SC01 datasheet v3.4* and *Getting Started Guide for WT32-SC01*
  (from Wireless Tag or your board seller).
- Espressif: [ESP32-WROVER-B datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-wrover-b_datasheet_en.pdf),
  [ESP32 Technical Reference Manual](https://www.espressif.com/sites/default/files/documentation/esp32_technical_reference_manual_en.pdf).
- Sitronix: *ST7796S* LCD controller datasheet.
- FocalTech: *FT5x06* capacitive touch controller datasheet / application note.

Pinout and board notes: [`docs/hardware.md`](docs/hardware.md).

## Quick start

### 1. Get a ROM and a boot disk

Put them in the `ROM/` folder next to `MacintoshESPEmulator/` (the folder is
git-ignored):

```
ROM/
├── LC-II.ROM          or Classic.ROM
└── OS_750_boot.dsk    bootable System 7.5.3 floppy (1.44 MB)
```

### 2. Build the custom boot disk

The default build embeds `ROM/custom_753.dsk`, a 2880K System 7.5.3 disk with
the desk accessories, control panels and the apps you choose. Build it from
images you supply with `tools/build_disk.py` (Python 3 + `pip install machfs`):

```bash
python3 tools/build_disk.py ROM/OS_750_boot.dsk \
    --hd "/path/System 7.5.3 HD.dsk" \
    --extra-file "/path/Game.dsk" "My Game" --silence-sounds \
    --prep --out ROM/custom_753.dsk
```

**Step-by-step guide, all options and the space budget:
[docs/custom-disk.md](docs/custom-disk.md).** Without the custom disk the
build falls back to the plain `OS_750_boot.dsk` floppy.

### 3. Choose the machine and the disk

Edit two lines in `MacintoshESPEmulator/main/CMakeLists.txt`:

```cmake
set(ROM_NAME "Classic.ROM")       # default; or "LC-II.ROM", "Color-Classic.ROM"
set(DISK_NAME "custom_753.dsk")   # default; falls back to "OS_750_boot.dsk" if absent
```

### 4. (Optional) Regenerate the Musashi opcode tables

The CPU core's generated files (`m68kops.c`, `m68kops.h`) are committed, so a
fresh clone builds as-is. Regenerate them only if you change `m68k_in.c`, with
any host C compiler (MinGW/WinLibs gcc on Windows, gcc on Linux/WSL):

```bash
cd MacintoshESPEmulator/components/musashi
gcc -o m68kmake m68kmake.c
./m68kmake . m68k_in.c
```

### 5. Build and flash

You need **ESP-IDF v6.1** (tested on Windows with the Espressif installer).
`MacintoshESPEmulator/build.bat` sets up the environment and runs `idf.py`.
It must run under `cmd.exe`, because ESP-IDF refuses MSYS/Git-Bash shells.
Edit the paths at the top of `build.bat` if your ESP-IDF is not in
`C:\esp\v6.1\esp-idf`.

```bat
cd MacintoshESPEmulator
build.bat set-target esp32
build.bat build
build.bat -p COM3 flash monitor
```

Replace `COM3` with your board's serial port (the board uses a CH340
USB-UART). On Linux/macOS, use `idf.py` from an ESP-IDF v6.1 shell instead of
`build.bat`. Full instructions: [`docs/build-guide.md`](docs/build-guide.md).

## Using the touch screen

The panel works like a laptop trackpad (relative movement, 1:1 with your finger):

| Gesture | Mac action |
|---|---|
| Swipe | Move the cursor (no click) |
| Tap (short, < 8 px movement) | Click |
| Two quick taps | Double-click |
| Tap, then touch again within 0.4 s and drag | Click-and-drag (menus, windows, icons) |
| Lift finger | End the movement or drag |

**Game mode:** when a full-screen game with no windows is in front (or the
cursor is hidden), taps are posted so that System 7 does not switch back to
the Finder. On the Classic, a game that draws outside 480×320 is shown as the
full 512×342 screen scaled to fit.

There is **no keyboard yet** (see the roadmap).

## Performance

Measured on the board with the default build (ESP32 at 240 MHz):

| | Classic (`Classic.ROM`, custom 2.88 MB disk) | LC II (`LC-II.ROM`, 1-bit) |
|---|---|---|
| Power-on to Finder | **about 63 s** | about 115 s (measured before the native speed-ups) |
| Finder idle | 100% real time (fast-forward skips about 83% of idle cycles) | 100% real time (60–65% skipped) |

While the Mac is busy (opening folders, drawing, loading resources) it runs
slower than a real Mac. On the PC host harness, the native speed-ups cut the
instructions executed to reach the Finder by 75% (Classic) and 81% (LC II).

## Limitations

- **No keyboard** input yet.
- **No sound** output.
- **Disk changes are lost at power-off** (the overlay lives in PSRAM). The
  disk image itself is read-only in flash.
- **One disk**, embedded in the firmware at build time (no SD card, no SCSI disk).
- **2 MB of Mac RAM.**
- **LC II colour** on the real panel is unverified, and the Monitors control
  panel does not recognise the emulated video yet.
- No serial ports, networking or AppleTalk (the SCC is a stub).
- Built and tested only on the WT32-SC01 v3.4.

## Roadmap

The ordered plan, with what is done and what is next (keyboard, board
verification, persistent disk, more native acceleration), is in
[`spec/next-steps-plan.md`](spec/next-steps-plan.md). History and
measurements are in [`spec/performance-roadmap.md`](spec/performance-roadmap.md).
Planned input hardware: an emulated ADB keyboard fed from an on-screen keyboard
or an external SPI/I2C keyboard, and a joystick on the expansion header.

## Learn how it works

New to the project? The [learning guide](docs/learn/README.md) explains, in
six chapters, how the ESP32 board and the two emulated Macs are built, how
the 68k CPU is emulated, and how the emulator was made fast enough, with the
measurements behind each step:
[hardware](docs/learn/01-hardware-esp32.md) ·
[emulated Macs](docs/learn/02-emulated-macs.md) ·
[CPU emulation](docs/learn/03-cpu-emulation.md) ·
[performance](docs/learn/04-performance.md) ·
[input, display, storage](docs/learn/05-input-display-storage.md) ·
[lessons learned](docs/learn/06-lessons-learned.md).

## Documentation

| Document | Contents |
|---|---|
| [`docs/learn/`](docs/learn/README.md) | Learning guide: hardware, emulated Macs, CPU, performance case study, lessons |
| [`docs/architecture-overview.md`](docs/architecture-overview.md) | Tasks, frame loop, source map, both machines |
| [`docs/build-guide.md`](docs/build-guide.md) | Toolchain, build variables, troubleshooting |
| [`docs/custom-disk.md`](docs/custom-disk.md) | How to build the custom boot disk (required for the default build) |
| [`docs/hardware.md`](docs/hardware.md) | Board, pins, memory limits |
| [`docs/memory-map.md`](docs/memory-map.md) | Classic and V8 address maps, low-memory globals |
| [`docs/musashi-integration.md`](docs/musashi-integration.md) | CPU core, 68030/PMMU stub, native speed-ups |
| [`docs/peripherals.md`](docs/peripherals.md) | VIA, Egret/Cuda, RTC, SWIM, disk path, SCSI, SCC |
| [`docs/display-pipeline.md`](docs/display-pipeline.md) | Renderer, native 480×320 modes, touch |
| [`MacintoshESPEmulator/host/README.md`](MacintoshESPEmulator/host/README.md) | PC host harness and profiler |
| [`CLAUDE.md`](CLAUDE.md) | Contributor guide: build switches, budgets, ground rules |

## Credits

- **[Musashi](https://github.com/kstenerud/Musashi)** 68000-family CPU emulator
  by Karl Stenerud, included in `MacintoshESPEmulator/components/musashi/`
  under its own MIT-style licence (see the notice at the top of `m68kcpu.c`).
  Local changes: 68020/030 EA decoding fix, PMMU instruction stub, IRAM
  placement, native trap helpers. The Musashi distribution also carries
  `m68kmmu.h` (R. Belmont, the MAME team) and John R. Hauser's SoftFloat
  (`softfloat/`), each with its own notice.
- **[MAME](https://github.com/mamedev/mame)**: used as the hardware reference
  for the V8 machines (address map, overlay, video, Egret/Cuda behaviour).
- **[Mini vMac](https://www.gryphel.com/c/minivmac/)** by Paul C. Pratt and
  contributors: the precedent for "screen hack" ROM patches and the Classic ROM
  patch approach.
- **[machfs](https://github.com/elliotnunn/machfs)** and
  **[macresources](https://github.com/elliotnunn/macresources)** by Elliot Nunn:
  the Python HFS and resource-fork libraries used by `tools/build_disk.py`.
- Espressif ESP-IDF and the `esp_lcd` driver.

## Licence

The project is released under the **MIT licence**: see [`LICENSE`](LICENSE).
It does not cover Apple ROMs, System software, disk images or Mac
applications. Third-party code in `MacintoshESPEmulator/components/musashi/`
(Musashi, `m68kmmu.h`, SoftFloat) keeps its own licence notices; see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## Legal

Apple ROMs and System software are copyrighted by Apple. **This repository
contains no ROM, no System software and no disk image**, and the `ROM/` folder
is git-ignored. You must supply your own ROM dump and boot disk, for example
from a Macintosh you own. Do not publish firmware binaries built from this
project: they embed the ROM and the disk image. Releases are source only.

Macintosh, Mac OS and System 7 are trademarks of Apple Inc. This project is not
affiliated with Apple.
