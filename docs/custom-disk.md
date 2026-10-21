# Creating the custom boot disk (`ROM/custom_753.dsk`)

The default firmware build embeds `ROM/custom_753.dsk`: a 2880 KB System 7.5.3
disk with the useful desk accessories and control panels plus whatever apps you
add. You have to build it yourself — the repository contains **no** Apple or
third-party software. If the file is missing, the build falls back to the plain
`ROM/OS_750_boot.dsk` floppy (System + Finder only).

> **Legal:** use only software you are entitled to use. Never commit the disk
> image or publish firmware that embeds it (`ROM/` is git-ignored).

## 1. What you need

| Item | Where it goes | Notes |
|---|---|---|
| A bootable **System 7.5.3 floppy** image | `ROM/OS_750_boot.dsk` | Supplies the System file, the Finder and the boot blocks (a lean System, ~1 MB). Bare HFS, 1440 KB. |
| A **full System 7.5.3 hard-disk image** | anywhere (passed with `--hd`) | Source of the extras: Calculator, Key Caps, Note Pad, Stickies, Scrapbook, Jigsaw Puzzle, • Shut Down, control panels, SimpleText. Its own (large) System file is *not* used. Bare HFS, Apple-partitioned or DiskCopy 4.2 images all work. |
| **Your apps** (optional) | anywhere | As MacBinary files (`.bin`) or inside HFS disk images (`.dsk`, `.img`, `.hfs`, `.image`). Black-and-white 68000 software is the safest choice. |
| **Python 3** + `machfs` | — | `pip install machfs` (pulls in `macresources`). |
| For `--prep`: a host C compiler + the ROMs | — | `--prep` boots the image once in the host harness (needs `ROM/LC-II.ROM`; WSL/Linux gcc). |

## 2. Build it

Minimal (System + extras, no apps):

```bash
python3 tools/build_disk.py ROM/OS_750_boot.dsk \
    --hd "/path/to/System 7.5.3 HD.dsk" \
    --prep --out ROM/custom_753.dsk
```

With apps — copy only the files you need from each image, silence sound
effects (the board has no audio output) and drop items you don't want:

```bash
python3 tools/build_disk.py ROM/OS_750_boot.dsk \
    --hd "/path/to/System 7.5.3 HD.dsk" \
    --extra-file "/path/to/Game.dsk" "My Game" \
    --extra-file "/path/to/Game Data.dsk" "My Game Data" \
    --extra "/path/to/MacBinaryApps/" \
    --silence-sounds \
    --exclude "Jigsaw Puzzle" \
    --prep --out ROM/custom_753.dsk
```

The script prints every file it copied and the free space left. Keep your own
command in a small script next to your images (for example in `ROM/`, which is
git-ignored) so you can rebuild the same disk later.

### Options

| Option | Effect |
|---|---|
| `--hd IMAGE` | Full System 7.5.3 install to copy the manifest items from |
| `--extra PATH` | Add every app/file from a `.bin` file, an HFS image or a folder of them (repeatable). Copies *everything* at the image's top level. |
| `--extra-file IMAGE NAME` | Add only the top-level item `NAME` from an HFS image (repeatable) — use it to leave read-mes, installers and unused data files out |
| `--copy IMAGE SRC DEST` | Copy one item from an HFS image to an exact place on the new disk (colon paths), e.g. an extension: `--copy Big.dsk "System Folder:Extensions:Color Picker" "System Folder:Extensions:Color Picker"` — colour games that use the Color Picker package (PACK 12) quit with "error of type 29" without it |
| `--silence-sounds` | Replace each `'snd '` resource of the added apps with a 44-byte silent sound. Resource IDs stay, so apps don't fail; typically saves a lot of space |
| `--exclude NAME` / `--include NAME` | Drop a manifest item / add an optional one (e.g. `WindowShade`) |
| `--size 2880K` \| `floppy` \| `<n>K/M` | Volume size. `2880K` is the default and the practical maximum (flash limit ≈ 2.89 MB). `floppy` = 1440 KB, dropping low-priority items until it fits |
| `--name NAME` | Volume name (default "Macintosh HD") |
| `--prep` | Boot the image once in the host harness so the Finder builds its desktop database (real icons on a read-only/flash disk). Recommended |
| `--no-volume-icon` | Don't add the built-in hard-disk volume icon |
| `--pylib DIR` | Extra Python path holding `machfs`/`macresources` if not installed |

The manifest (which Apple items are copied, their priorities and window
positions) is the `MANIFEST` list near the top of `tools/build_disk.py`.

## 3. Space budget

The flash has room for a disk of about **2.89 MB**, so `2880K` is the maximum.
The lean System + Finder take ~1.3 MB, the default extras ~0.8 MB, and the
Finder's desktop database (built by `--prep`) ~0.1 MB. Watch the "free" figure
the script prints:

- Leave **~60–100 KB free** if you want to save files: the disk is writable
  during a session (changes live in PSRAM and are lost at power-off).
- Free space with `--exclude` (e.g. Note Pad, Monitors, Memory, Sound, Jigsaw
  Puzzle) and `--silence-sounds`.

## 4. Use it

`ROM_NAME`/`DISK_NAME` defaults in `MacintoshESPEmulator/main/CMakeLists.txt`
are `Classic.ROM` + `custom_753.dsk`, so just build and flash:

```bash
cmd.exe /C "E:\Personal\Macintosh\MacintoshESPEmulator\build.bat build"
cmd.exe /C "E:\Personal\Macintosh\MacintoshESPEmulator\build.bat flash"
```

The build log shows `Embedding disk image: custom_753.dsk (2949120 bytes)`.
To try the disk on the PC first: `cd MacintoshESPEmulator/host && make
MAC_ROM_LC_II=0 DISK_NAME=custom_753.dsk && ./build/macemu --frames 20000
--shot-every 5000` (see `host/README.md`).

## 5. Tips

- **Full-screen games** that expect the classic 512×342 screen can be added to
  `full_screen_apps[]` in `MacintoshESPEmulator/main/mac_memory.c` (prefix of
  the application name); they then get the full 512×342 view
  ([display-pipeline.md](display-pipeline.md)).
- Apps that need a **keyboard** can't be played yet (keyboard input is planned).
- Colour apps run on the LC II build (`ROM_NAME "LC-II.ROM"`); the Mac Classic
  build is black-and-white.
- If an app quits or misbehaves, check it on the host harness first — it is
  deterministic and gives screenshots and traces.
