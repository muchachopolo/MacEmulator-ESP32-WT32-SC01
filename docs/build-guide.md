# Build guide

See also: [README quick start](../README.md#quick-start),
[host harness](../MacintoshESPEmulator/host/README.md),
[hardware](hardware.md).

## Prerequisites

1. **ESP-IDF v6.1.** The project is developed on Windows with the Espressif
   installer (EIM): IDF at `C:\esp\v6.1\esp-idf`, tools at `C:\Espressif\tools`.
   On Linux/macOS, use a normal ESP-IDF v6.1 install and run `idf.py` directly.
2. **A host C compiler** to generate the Musashi opcode tables (MinGW/WinLibs
   gcc on Windows, e.g. `winget install BrechtSanders.WinLibs.POSIX.UCRT`, or
   gcc on Linux/WSL).
3. **A ROM and a boot disk** in `ROM/` next to `MacintoshESPEmulator/`
   (git-ignored, user-supplied): `LC-II.ROM` and/or `Classic.ROM`
   (`Color-Classic.ROM` optional), and a System 7.5.3 boot disk
   (`OS_750_boot.dsk`, or a custom image, see below).
4. Optional: WSL/Linux with gcc and make for the host harness and
   `build_disk.py --prep`; Python 3 with `machfs` for `build_disk.py`.

## Regenerate the Musashi opcode tables (only after editing `m68k_in.c`)

`m68kops.c` and `m68kops.h` are generated from `m68k_in.c` and committed, so a
fresh clone builds without this step. To regenerate:

```bash
cd MacintoshESPEmulator/components/musashi
gcc -o m68kmake m68kmake.c
./m68kmake . m68k_in.c
```

`m68k_in.c` already puts `EXT_RAM_BSS_ATTR` on the two big tables, so the
output is ready to use (without it `.dram0.bss` overflows by ~434 KB).
`m68kmake` does not read `m68kconf.h`: changing a config switch needs no
regeneration.

## Build variables

In `MacintoshESPEmulator/main/CMakeLists.txt`:

| Variable | Committed default | Effect |
|---|---|---|
| `ROM_NAME` | `Classic.ROM` | `Classic.ROM` → Classic build (`MAC_ROM_LC_II=0`); `LC-II.ROM` → LC II (`MAC_ROM_LC_II=1`, `MAC_V8_CUDA=0`); `Color-Classic.ROM` → `MAC_V8_CUDA=1` |
| `DISK_NAME` | `custom_753.dsk` | Disk image in `ROM/` to embed; `DISK_SYM` is derived from it. Falls back to `OS_750_boot.dsk` if the custom disk hasn't been built; if neither exists the Mac boots to the "?" disk icon |

`MUSASHI_HOT_IRAM` (ON) is set in `components/musashi/CMakeLists.txt`. All
other switches are C defines; the list with defaults is in
[`CLAUDE.md`](../CLAUDE.md#build-switches).

## Building on Windows (`build.bat`)

ESP-IDF rejects MSYS/MinGW shells (Git Bash, MSYS2). `build.bat` sets
`IDF_PATH`, `IDF_TOOLS_PATH`, the Python venv and `PATH`, clears the MSYS
variables, sets `ESP_IDF_VERSION=6.1.0`, and runs `idf.py` with your
arguments. Edit the paths at its top if your install differs.

From `cmd.exe`:

```bat
cd MacintoshESPEmulator
build.bat set-target esp32      (first time, and after fullclean)
build.bat build
build.bat -p COM3 flash
build.bat -p COM3 monitor
build.bat size
build.bat fullclean
```

From a bash shell (Git Bash or WSL), wrap the call:

```bash
cmd.exe //C "E:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat build"
```

The board's USB-UART is a CH340; install its driver if no COM port appears.
`tools/serial_watch.py` (pyserial, COM3) captures the log to
`tools/serial_watch.log`. Close it before flashing: it holds the port.

## Project configuration

`sdkconfig.defaults`:
- `CONFIG_IDF_TARGET="esp32"`, CPU 240 MHz, FreeRTOS 1000 Hz.
- Flash **4 MB, 80 MHz, QIO** (QIO measured +1.1% vs DIO; the image header
  still says "dio" and the bootloader switches to QIO).
- PSRAM quad 80 MHz, `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` (for
  `EXT_RAM_BSS_ATTR`).
- `CONFIG_COMPILER_OPTIMIZATION_PERF=y`, custom partition table.

`partitions.csv`: `nvs` 0x9000 (24 KB), `phy_init` 0xF000, `factory` app at
0x10000, size **0x3F0000**. With the 2880K custom disk the firmware is about
0x3E18F0, so there is almost no room left; with the 1.44 MB floppy there is
about 1.4 MB free.

ROM and disk are embedded with `EMBED_FILES` and linked into `.rodata` as
`_binary_<ROM_NAME>_start/end` and `_binary_<DISK_SYM>_start/end` (for
example `_binary_OS_750_boot_dsk_start`).

## Custom boot disk

`tools/build_disk.py` builds a bare HFS System 7.5.3 volume. It contains no
Apple data: everything is copied from images you supply.

- From the **base** floppy: System, Finder, Finder Preferences, boot blocks.
- From `--hd` (a full 7.5.3 hard-disk image, bare HFS or partitioned): the
  `MANIFEST` items (Calculator, Key Caps, Note Pad, Scrapbook, Shut Down,
  control panels, SimpleText, ...).
- From `--extra` (repeatable): your apps as MacBinary `.bin` files, HFS images
  or folders of them, copied to `Applications`.
- A built-in hard-disk volume icon (`--no-volume-icon` skips it), Finder
  window positions that fit 480×320, and a Control Panels alias in the Apple menu.

```bash
pip install machfs            # or --pylib DIR with an unpacked machfs/macresources
python3 tools/build_disk.py ROM/OS_750_boot.dsk --hd "/path/System 7.5.3 HD.dsk" \
    [--extra ~/macapps] [--size 2880K|floppy|<n>K] [--exclude NAME] [--include NAME] \
    [--name "Macintosh HD"] [--prep] [--out ROM/custom_753.dsk]
```

- `--size`: default 2880K, the most that fits the flash budget (~2.89 MB =
  app partition minus code and the 512 KB ROM). `floppy` = 1440K, dropping the
  lowest-priority manifest items until the volume fits.
- `--prep` (recommended; WSL/Linux with gcc): boots the new image once in the
  host harness (LC II build; `LC-II.ROM` must be in the output folder) with the
  disk writable, lets the Finder build the desktop database, and shuts down
  cleanly. Without it the Finder shows generic icons.
- `--include WindowShade`: optional, because its INIT corrupts memory on the
  Classic build (General Controls then crashes). It works on the LC II.

Keep your own build command (which apps, which exclusions) in a local script next to the images in `ROM/`, which is git-ignored. `DISK_NAME "custom_753.dsk"` is already the default (host: `make DISK_NAME=custom_753.dsk
BUILD=build_cd`). Any size up to the flash budget mounts through the floppy
path (.Sony drive 1) on both machines.

## Host harness

```bash
cd MacintoshESPEmulator/host
make -j16           # build/macemu (LC II by default; MAC_ROM_LC_II=0 for the Classic)
make run            # 3000 frames, PNG screenshots in out/, RAM dump
make TRACE=1 -j16   # per-instruction trace and profiler
```

All options: [`host/README.md`](../MacintoshESPEmulator/host/README.md).

## Troubleshooting

| Symptom | Fix |
|---|---|
| "MSYS/MinGW shell is not supported" | Run through `cmd.exe` and `build.bat` |
| "Musashi: m68kops.c and/or m68kops.h not found" | Generate them (above) |
| `.dram0.bss` overflow at link | `EXT_RAM_BSS_ATTR` missing on the Musashi tables, or `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` off |
| IRAM overflow | Trim `components/musashi/hot_iram.lf` or set `MUSASHI_HOT_IRAM` OFF |
| App partition overflow | Disk image too big: use a smaller `--size` or the 1.44 MB floppy |
| "?" disk icon | `DISK_NAME` file not found in `ROM/` at configure time (re-run the build after adding it) |
| Flash fails, port busy | Close serial monitors / `serial_watch.py` |
| New GCC warnings in Musashi | Add `-Wno-*` flags in `components/musashi/CMakeLists.txt` |
