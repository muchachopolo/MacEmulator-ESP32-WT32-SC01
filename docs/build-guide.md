# Build Guide

## Prerequisites

1. **ESP-IDF v6.0** installed via EIM at `C:\esp\v6.0\esp-idf`
   - Tools at `C:\Espressif\tools`
   - Python venv at `C:\Espressif\tools\python\v6.0\venv`

2. **MinGW GCC** (only needed if regenerating Musashi opcode tables)
   - Install: `winget install BrechtSanders.WinLibs.POSIX.UCRT`
   - Used to compile `m68kmake.c` as a host tool

3. **ROM files** in `ROM/` directory:
   - `Classic.ROM` (512KB) — **required**
   - `OS_750_boot.dsk` (1.44MB) — optional boot floppy

## Building

ESP-IDF v6.0 rejects MSYS/MinGW shells. All builds MUST go through `build.bat` via `cmd.exe`:

```bash
# Build
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat build"

# Flash (connect board first)
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat flash"

# Monitor serial output
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat monitor"

# Flash and monitor in one step
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat flash monitor"

# Check binary size
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat size"
```

### After a Full Clean

```bash
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat fullclean"
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat set-target esp32"
cmd.exe //C "D:\\Personal\\Macintosh\\MacintoshESPEmulator\\build.bat build"
```

## build.bat Explained

The build script (`MacintoshESPEmulator/build.bat`) does the following:

1. Sets `IDF_PATH`, `IDF_TOOLS_PATH`, `IDF_PYTHON_ENV_PATH`, etc.
2. Clears MSYS/MinGW environment variables (`MSYSTEM`, `MSYS`, etc.) to prevent ESP-IDF from rejecting the shell
3. Sets `ESP_IDF_VERSION=6.0.0` (required by idf_component_manager)
4. Configures `PATH` with all ESP-IDF tool directories
5. Runs `idf.py` with whatever arguments are passed

## Project Configuration

### sdkconfig.defaults

Key settings:
- `CONFIG_IDF_TARGET="esp32"` — ESP32 (not S2/S3/C3)
- `CONFIG_ESPTOOLPY_FLASHSIZE_4MB` — 4MB flash (actual hardware, despite 16MB label)
- `CONFIG_SPIRAM=y` — Enable 8MB PSRAM
- `CONFIG_SPIRAM_SPEED_80M=y` — PSRAM at 80MHz
- `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` — Allow `EXT_RAM_BSS_ATTR`
- `CONFIG_PARTITION_TABLE_CUSTOM=y` — Custom partition table
- `CONFIG_COMPILER_OPTIMIZATION_PERF=y` — Optimize for speed

### Partition Table (partitions.csv)

```
nvs,       data,  nvs,      0x9000,   0x6000
phy_init,  data,  phy,      0xf000,   0x1000
factory,   app,   factory,  0x10000,  0x3F0000   (~3.9MB of 4MB flash)
```

Single factory partition. Firmware (2.4MB) includes embedded ROM + disk image with 41% free.

### Embedded Files

ROM and disk images are embedded into flash via ESP-IDF's `EMBED_FILES` in `main/CMakeLists.txt`:
- `Classic.ROM` → accessible as `_binary_Classic_ROM_start/end`
- the disk image named by `DISK_NAME` (default `OS_750_boot.dsk`) → `_binary_<DISK_NAME as C identifier>_start/end` (e.g. `_binary_OS_750_boot_dsk_start`); the C code gets the symbol stem as `DISK_SYM`

These land in `.rodata` (flash), not DRAM.

### Custom boot disk (larger than a floppy)

`tools/build_disk.py` builds a bare HFS System 7.5.3 volume from images you supply
(no Apple data is in the repo). It takes System, Finder, Finder Preferences and the
boot blocks from a bootable 7.5.3 floppy, copies the items listed in its `MANIFEST`
(Apple menu items, control panels, SimpleText) from a full 7.5.3 hard-disk image,
and optionally adds your own apps (MacBinary `.bin` files or HFS images) to
`Applications`. Needs `pip install machfs` (or `--pylib DIR` with an unpacked copy).

```bash
python3 tools/build_disk.py ROM/OS_750_boot.dsk --hd "/path/System 7.5.3 HD.dsk" \
    [--extra ~/macapps] [--size 2880K|floppy] [--exclude NAME] [--include NAME] \
    [--prep] [--out ROM/custom_753.dsk]
```

- `--size`: default 2880K (the flash budget allows ~2.89 MB: app partition 0x3F0000
  minus ~1.10 MB code and the 512 KB ROM). `floppy` = 1440K, dropping the
  lowest-priority manifest items until it fits.
- `--prep` (recommended, WSL/Linux): boots the new image once in the host harness
  (LC II build, `LC-II.ROM` must be in the output folder) with the disk writable
  (`macemu --disk-rw`), lets the Finder build the desktop database (real icons on the
  read-only volume), then shuts down cleanly. Without it the Finder shows generic icons.
- WindowShade is `optional` (`--include WindowShade`): its INIT corrupts memory on the
  Classic build (opening General Controls then crashes); it works on the LC II.

Then set `DISK_NAME "custom_753.dsk"` in `main/CMakeLists.txt` (host:
`make DISK_NAME=custom_753.dsk`, with its own `BUILD=` dir). The image is served through
the floppy path (.Sony drive 1) at any size; it is read-only: writes fail with wPrErr
and the volume mounts locked (Note Pad says "the file could not be opened"; Stickies
opens without saving).

## Regenerating Musashi Opcode Tables

Only needed if you modify `m68k_in.c` (e.g., to add custom instructions):

```bash
cd MacintoshESPEmulator/components/musashi
gcc -o m68kmake m68kmake.c
./m68kmake . m68k_in.c
```

This generates `m68kops.c` (~836KB, ~37,000 lines) and `m68kops.h` from the opcode DSL.

**After regeneration**: You must re-add the `EXT_RAM_BSS_ATTR` patch to `m68kops.c` (search for `m68ki_instruction_jump_table` and `m68ki_cycles` near line 34715).

## Host Harness (PC build for debugging)

`MacintoshESPEmulator/host/` builds the same emulator core for x86 Linux/WSL with ESP-IDF
shims, so boot problems can be debugged in seconds:

```bash
cd MacintoshESPEmulator/host
make -j16                 # build/macemu
make run                  # 3000 frames, PNG screenshots in out/, RAM dump
make TRACE=1 -j16         # build_trace/macemu with --trace N (per-instruction log)
```

See `host/README.md` for all options. The host has no touch, runs on a single thread, and
uses plain RAM instead of PSRAM, so speed and timing must still be checked on the board.

## Flash Mode

`sdkconfig.defaults` selects QIO flash (measured +1.1% emulation speed vs DIO on this
board). ESP-IDF still writes "dio" in the image header; the bootloader switches to QIO.

## Troubleshooting

### "MSYS/MinGW shell is not supported"
You're running from Git Bash or similar. Use `cmd.exe //C build.bat` instead.

### "ESP_IDF_VERSION not set"
The `build.bat` already sets this. If running manually, add `set ESP_IDF_VERSION=6.0.0`.

### DRAM overflow at link time
Musashi's large tables (580KB) must be in PSRAM. Ensure:
- `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` in sdkconfig
- `EXT_RAM_BSS_ATTR` on `m68ki_instruction_jump_table` and `m68ki_cycles` in `m68kops.c`

### GCC 15.2.0 warnings in Musashi
The Xtensa GCC 15.2.0 is stricter than older versions. Warning suppressions are in `components/musashi/CMakeLists.txt`. If new warnings appear, add the corresponding `-Wno-*` flag there.
