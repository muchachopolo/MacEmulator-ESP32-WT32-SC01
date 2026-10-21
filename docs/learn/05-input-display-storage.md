# 5. Input, display and storage

A Mac needs a mouse, a screen and a disk. The board has a touch panel, an
SPI LCD and a flash chip. This chapter explains how each gap is bridged.

Reference: [`docs/display-pipeline.md`](../display-pipeline.md) (renderer,
patched immediates, gesture constants) and
[`docs/peripherals.md`](../peripherals.md) (disk path, overlay).

## Touch → mouse

### The pipeline

```
 core 0                                  core 1 (once per emulated frame)
 ──────                                  ─────────────────────────────────
 touch task, every 16 ms                 touch_consume()
   display_get_touch() over I2C            movement since last call
   axis swap + horizontal invert           + ordered button transitions
   gesture state machine ───queue───►            │
   (spinlock-guarded)                            ▼
                                         cursor position → OS globals
                                         button → Classic: PostEvent mirror
                                                  LC II: Egret ADB mouse
                                         game mode / full view decisions
```

Sampling runs on its own task (`TOUCH_SAMPLER_TASK`) so it keeps a steady
~60 Hz while the emulator's frame rate varies. Each button state is held for
at least two emulated frames, so a quick tap is never lost between two
frames.

### Gestures: a trackpad, not a touchscreen

An absolute touchscreen ("click where the finger is") would need a finger
smaller than a Mac menu item and would hide what it touches. The panel acts
as a **relative trackpad** instead:

| Gesture | Mac action |
|---|---|
| Swipe (> 8 px) | Move the cursor 1:1 with the finger, no button |
| Tap (< 300 ms, < 8 px) | Click |
| Second touch within 400 ms of a tap | Click-and-drag (button held while moving) |
| Slow nudge (3 px after 300 ms) | Counts as movement, for fine positioning |

Once a touch counts as a swipe, the movement since touch-down is applied, so
there is no dead zone that swallows the first 8 px.

### Moving the cursor

The Mac's cursor lives in low-memory globals. The emulator writes the new
position directly into `MTemp` ($0828), `RawMouse` ($082C) and `Mouse`
($0830), then sets `CrsrNew` ($08CE) so the cursor VBL task redraws it.
Because `MTemp` equals `RawMouse`, the OS sees no raw delta to accelerate:
the cursor follows the finger exactly 1:1. Both machines position the cursor
this way.

Two rules learned the hard way:

- **Write `CrsrNew` only when the position changes.** Writing it every frame
  forced cursor redraws that crashed the resource decompressor while INITs
  and the Finder were loading.
- **Only after `WMgrPort` ($09DE) is valid.** Before the Window Manager is
  up, there is no cursor to move.

### Clicking: the event queue, not a flag

On ADB Macs, applications learn about clicks from **events** in the OS event
queue (`GetNextEvent`, `WaitNextEvent`, `ModalDialog`). The `MBState` flag
alone is not enough. The two builds deliver clicks differently:

| | Classic (`CLASSIC_MOUSE_POSTEVENT`) | LC II |
|---|---|---|
| Mechanism | A C mirror of the ROM's ADB mouse handler ($403A54): update `RndSeed`, `MBTicks`, `MBState`, then the equivalent of `PostEvent` into the System's own event buffer, honouring `SysEvtMask` | The emulated **ADB mouse** in the Egret HLE reports the button (with zero movement); the ROM's own ADB driver posts the events |
| Safety | Posts only while the 68k interrupt mask is 0, so a post never lands inside an OS queue routine | Movement that follows a button change waits until that change has gone out in an ADB packet, so the order is preserved |
| Also | `MBState` written as a whole byte ($00/$80); VIA PB3 follows the button | |

The Classic mirror replaced an older approach that inserted fixed queue
elements at $C0/$D8; mirroring the ROM's own path removed that special case.

### Game mode

Full-screen games such as a full-screen game own no windows. System 7's Process
Manager sends a `mouseDown` to the layer under the point. With no game window
there, that is the Finder's layer, so a tap **switched the game out**: it
looked as if the game had frozen or vanished.

Game mode (`TOUCH_GAME_MODE`) detects a windowless, non-Finder application
in front (`WindowList` $09D6 = NULL, scored over about one second so short
Finder background slices do not flip it) or a **hidden cursor**. In that
mode, clicks are posted with their location in the menu bar (v = 1), which
the Process Manager leaves to the front application. `MBState` is set too,
for games that poll `_Button`.

The hidden-cursor test was added after a board test: the idle fast-forward
ends frames inside the Process Manager's idle loop, where the "current
process" seen at frame end is rarely the game. `CrsrState` is global, not
swapped per process, so it is a reliable signal.

### Game full view (Classic)

Some Classic games draw a 512×342 playfield regardless of what the screen
size says. In the native 480×320 mode the right and bottom edges would be cut
off. With `GAME_FULL_VIEW`, when game mode holds (0.5 s to enter, 1 s to
leave) and the game has drawn into the unused margin (columns 480–511, rows
320–341), the renderer switches to a scaled view of the whole 512×342. The
cursor bounds widen and finger movement is scaled to match. Leaving the game
restores the crisp 1:1 view.

## Display

### From Mac pixels to LCD pixels

```
 Classic: 1-bit framebuffer in Mac RAM      LC II: VRAM $F40000 (1–16 bpp) + CLUT
                \                                   /
           mac_video_render() / v8_render()  (core 0)
                for each 20-line strip (16 strips):
                  hash the source bytes; unchanged? → skip
                  convert via lookup table → RGB565 line buffer (19.2 KB, DMA RAM)
                  display_draw() → SPI DMA at 40 MHz, wait for "done"
                                 │
                          ST7796S 480×320
```

### Lookup tables instead of per-pixel logic

A 1-bit Mac byte holds 8 pixels. The Classic renderer uses a 256 × 8 table
(`byte_lut`): for each possible byte, the 8 RGB565 pixels it becomes. A row
of 60 bytes (480 pixels) is 60 table lookups and copies, with no per-bit
branches.

The LC II uses the same idea for every depth up to 8 bpp: one 4 KB table
maps a VRAM byte to up to 8 pixels through the current palette (the CLUT
written by the Mac at $F24000). The table is rebuilt only when the depth or
the palette changes. The palette is stored pre-byte-swapped: the panel expects the
high byte of each RGB565 pixel first, and the ESP32 is little-endian.

In native 480×320 mode both renderers copy 1:1. The scaled paths (stock
512×342 or other modes) use precomputed column and row tables.

### Dirty strips: do not send what did not change

A full frame is 300 KB of SPI traffic and about **61 ms** on the bus, so a
full-screen redraw runs at roughly 15 fps. Most of the time almost nothing
on screen changes. With `MAC_VIDEO_DIRTY`, each 20-line strip's source bytes
are hashed (an FNV-style hash, word reads). Only strips whose hash changed
are converted and sent. A full redraw is forced when the base address,
depth, geometry or palette changes.

When nothing changed, the video task sleeps for one tick instead of
re-reading the framebuffer: **an idle Finder costs 0 fps of SPI**, and core
0 stops taking PSRAM bandwidth from the emulator on core 1. A moving cursor
or a menu touches only one or two strips.

### Chunked DMA

The line buffer must be in internal DMA-capable RAM, which is scarce. 20
lines × 480 pixels × 2 bytes = 19.2 KB is the compromise. The renderer
converts a strip, starts the DMA, and waits on a semaphore given by the SPI
"done" callback before reusing the buffer. A planned improvement is a second
buffer (+19 KB) so conversion and transfer overlap.

## Storage

### The disk lives in flash

The disk image is embedded in the firmware (`DISK_NAME`, see
[chapter 1](01-hardware-esp32.md)) and always appears as an internal floppy
in drive 1. It is never copied to PSRAM; reads go straight to flash through
the cache. Volumes up to the flash budget (2880 KB) mount through the same
floppy path, so a "big floppy" serves as a small hard disk.

### Serving reads: hooks on the .Sony driver

The Mac's floppy driver is called `.Sony`. Emulating it through the SWIM
chip byte by byte works but costs many 68k instructions per byte. Instead,
the emulator answers whole requests at the driver level:

| | Classic | LC II |
|---|---|---|
| Hook | **Fetch intercepts** at the Device Manager's `.Sony` entry points ($40315C read, control/status, $4031CC write) | **JPrime hook**: the driver jumps through the low-memory vector `JPrime` ($0226) |
| How | The fetch callback runs C, fills the request, and returns `RTS` as the opcode ([chapter 3](03-cpu-emulation.md)) | After the driver's Open, `JPrime` is pointed at a 12-byte stub in the unused window $FE0000. The stub's `TST.B $FE0100` read runs `v8disk_prime()` in C; it then jumps to `IODone` |
| Safety nets | A "spin rescue" at $4031BE completes asynchronous reads that missed the entry; asynchronous completion routines are called (this fixed a Shut Down freeze) | None needed: nothing runs per instruction |

On the LC II the ROM's own driver still does Open, Control, Status and disk
detection through the emulated SWIM. Only the data transfer (Prime) is
replaced. The SWIM sense lines therefore have to be right, and one of them
caused a memorable bug ([chapter 6](06-lessons-learned.md)).

### Writing: a copy-on-write overlay in PSRAM

Flash cannot be written cheaply while the emulator runs from it, so writes go
to an overlay (`mac_disk_overlay.c`, `DISK_WRITABLE`):

```
 read block n:   overlay has n? ── yes ─► overlay slot
                       │ no
                       └──────────────► flash image
 write block n:  same as flash? ─ yes ─► nothing stored
                       │ no
                       └──────────────► take/overwrite a slot
                 pool full? ──► whole request fails with dskFulErr (−34)
```

Each written 512-byte block gets a slot in a pool of 384 KB (Classic) or
256 KB (LC II, whose VRAM already uses 512 KB of PSRAM), plus a 2-byte
index entry per disk block. A request that does not fit fails as a whole,
so the volume never holds half a write. The volume mounts unlocked and
saving files works, but **everything is lost at power-off**. A planned
"level 2" would flush to a flash partition and keep PRAM in NVS.

### Building a custom disk

`tools/build_disk.py` (with Elliot Nunn's `machfs`) builds a 2880 KB HFS
System 7.5.3 volume from images you supply: System and Finder from the boot
floppy, Apple menu items and control panels from a full install, plus your
own applications. Finder window positions are set to fit 480×320. With
`--prep` it boots the new disk once in the host harness, with the overlay
large enough, so the Finder builds its desktop database (otherwise icons are
generic), and writes the result back. Details:
[`docs/build-guide.md`](../build-guide.md#custom-boot-disk).

## Takeaways

- The touch panel is a trackpad; position goes straight into OS globals,
  clicks go through the event queue the way the ROM would post them.
- Full-screen games needed special click routing, because System 7 routes
  clicks by window layer.
- The LCD bus is slow, so the renderer sends only what changed, through
  lookup tables and small DMA strips.
- Disk reads are served at the driver level from flash; writes land in a
  PSRAM overlay that is lost at power-off.

Next: [6. Lessons learned](06-lessons-learned.md).
