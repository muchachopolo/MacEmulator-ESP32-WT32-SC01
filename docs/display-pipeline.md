# Display pipeline and touch input

Renderer: `main/mac_video.c` (task `mac_video_task`, core 0). LCD driver:
`components/display/display.cpp`. Touch: `main/mac_touch.c` +
`main/mac_emulator.c`. V8 video hardware: `main/mac_v8video.c`
([peripherals](peripherals.md#v8-video)).

## Overview

```
Classic: 1-bit framebuffer in Mac RAM (ScrnBase)     LC II: VRAM $F40000 (1-16 bpp) + CLUT
                     \                                 /
                      mac_video_render() / v8_render()
                      20-line strip → RGB565 line buffer (19.2 KB, internal DMA RAM)
                      unchanged strips skipped (MAC_VIDEO_DIRTY)
                                     │
                      display_draw() → esp_lcd SPI DMA, 40 MHz, SPI2
                                     │
                              ST7796S 480×320 RGB565
```

Each strip is converted, sent by DMA, and waited on (a semaphore given by the
SPI done callback) before the buffer is reused. With `MAC_VIDEO_DIRTY` (default
1) each strip's source bytes are hashed; only changed strips are converted and
sent, and the task sleeps a tick when nothing changed. An idle Finder costs one
framebuffer read per frame instead of 300 KB of SPI traffic. A full frame
(16 strips) takes about 61 ms of SPI time, so full-screen animation runs at
roughly 15 fps; typical UI updates touch only a few strips.

## Classic: native 480×320 (`CLASSIC_NATIVE_LCD`, default 1)

The Classic ROM is patched so the Mac uses a 480×320 visible screen (the Mini
vMac "screen hack" precedent) and the renderer copies 1:1 through a
byte → 8 pixel lookup table (bit 7 = leftmost, 1 = black). The framebuffer
layout is unchanged (ScrnBase = MemTop − $5900, rowBytes 64, 512×342);
columns 480–511 and rows 320–341 are an unused margin. Patched immediates (ROM
file offsets, applied in `mac_memory.c`'s ROM read paths):

| Offset | Routine | Change |
|---|---|---|
| $0060A / $0060E | InitCrsr globals (CrsrPin $0834) | 342/512 → 320/480 |
| $18F9A / $18FA0 | ScrnSize (JScrnSize $0810) | 342/512 → 320/480 |
| $18FB4 | _ScrnBitMap (InitGraf → screenBits.bounds) | 342 → 320 |

The boot screens (happy Mac, "Welcome to Macintosh", shutdown box) are
re-centred with further immediate patches. Two 512-pixel constants remain: the
sad-Mac screen ($401092) and the disk-switch alert ($4044F8).

Native mode needs the corrected Classic VIA timer model (`CLASSIC_VIA_INTERP`
+ `CLASSIC_VIA_ECLOCK`); without it a Time Manager calibration divides by
zero once the boot timing shifts. `CLASSIC_NATIVE_LCD=0` restores the stock
512×342 screen, drawn through the scaled path.

### Scaled path and game full view

The scaled path maps 512×342 onto 480×320: horizontally it emits 15 pixels per
2 source bytes (display x = 15k+j samples source x = 16k+j, so the last pixel
of every 16 is dropped); vertically rows are picked through `y_src_table`.

**Game full view** (`GAME_FULL_VIEW`, default 1, Classic native build only).
While a full-screen game runs (touch game mode, 0.5 s to enter, 1 s to leave)
and has drawn into the margin (compared with a baseline taken outside games),
the emulation loop sets `mac_full_view` and the renderer switches to the
scaled path (with a forced full redraw on each switch). CrsrPin becomes
0,0,342,512 and touch deltas are scaled by 512/480 × 342/320; leaving the game
restores the crisp 1:1 view, CrsrPin and the cursor clamp.
`GAME_FULL_SCREENBITS`: apps listed in `full_screen_apps[]` (`mac_memory.c`,
CurApName prefix: a full-screen game, a full-screen game) get the stock 512×342 from
ScrnSize/_ScrnBitMap, because a full-screen game centres its 512×342 field on
screenBits (with 480×320 it would be cut 16 px left/right and 11 rows
top/bottom).

## LC II: native 480×320 colour (`V8_NATIVE_LCD`, default 1)

The V8 video driver offers 512×384 for the built-in monitor (sense code 2,
"12-inch RGB"). `v8video_patch_rom()` patches the 512×384 VPBlocks in the
declaration ROM (LC II $7EC18–$80000, 13 blocks) to 480×320 and recomputes
the Slot Manager CRC. The patched copy (~5 KB PSRAM) is served on byte reads;
the flash image stays intact for the ROM's startup checksum. The main device
rect, pixel map bounds and CrsrPin are all 480×320.

`v8_render()` reads VRAM (rowBytes 1024) at 1, 2, 4, 8 or 16 bpp, maps
indexed pixels through the CLUT (MAME's index rule) and draws 1:1 at 480×320
(other sizes are scaled). The palette is pre-byte-swapped for esp_lcd. The LC
II starts at 1-bit; colour depths are chosen in the Monitors control panel
(which currently says it "cannot be used" with the emulated video). **The
panel's colour order has not yet been verified on the board** (the driver
uses BGR element order).

## Panel configuration

ST7796S driven through `esp_lcd_new_panel_st7789()` (compatible command set),
SPI2 at 40 MHz, RGB565, BGR element order, landscape via swap-XY + mirror X/Y,
backlight on GPIO 23. Pins: [hardware](hardware.md).

## Touch input

The FT5x06 is used as a **relative trackpad**; the cursor follows the finger
1:1 (no acceleration) on both machines.

### Sampling (`mac_touch.c`)

- `TOUCH_SAMPLER_TASK=1` (ESP32 default): a FreeRTOS task on core 0 reads the
  panel over I2C at ~60 Hz (`TOUCH_POLL_US` 16 ms), independent of emulation
  speed. The emulation loop calls `touch_consume()` once per frame and gets
  the movement since the last call plus ordered button transitions
  (spinlock-guarded; each button state is held ≥ 2 emulated frames).
- `TOUCH_SAMPLER_TASK=0` (host default): `touch_poll_inline()` samples from the
  loop every 16 ms of wall clock.
- Touch axes are swapped and the horizontal axis inverted relative to the display.

### Gestures

| Gesture | Rule | Result |
|---|---|---|
| Swipe | movement > 8 px (`TAP_MOVE_THRESH`); after 300 ms a 3 px nudge also counts | Cursor moves; the movement since touch-down is applied (no dead zone) |
| Tap | < 300 ms (`TAP_TIME_US`) and < 8 px | Click (down + up) |
| Tap-then-drag | second touch within 400 ms (`TAP_DRAG_WINDOW_US`) of a tap | Button held while dragging |
| Lift | | Ends movement / drag |

### Applying it to the Mac (`mac_emulator.c`)

- **Position** (both machines): MTemp $0828, RawMouse $082C and Mouse $0830
  (Point = Y.hi Y.lo X.hi X.lo) are set to the new position and CrsrNew $08CE
  is set (if CrsrCouple $08CF) — only when the position changed, and only once
  `mac_post_boot` is set and WMgrPort ($09DE) is valid. Writing CrsrNew every
  frame crashed the resource decompressor during boot. MTemp = RawMouse means
  the cursor VBL task sees no raw delta, so there is no acceleration.
- **Button, Classic** (`CLASSIC_MOUSE_POSTEVENT=1`): a C mirror of the ROM's
  ADB mouse handler ($403A54): RndSeed += Ticks − MBTicks, MBTicks = Ticks,
  MBState = $00/$80, then PostEvent ($403EE6) and Enqueue mirrored in C into
  the System's own SysEvtBuf (honours SysEvtMask). Posts happen only while the
  68k interrupt mask is 0, so they never land inside an OS queue routine.
  MBState is also written as a whole byte every frame, and VIA PB3 (active
  low) follows the button. Clicks must reach the event queue: on ADB Macs
  GetNextEvent/WaitNextEvent read the queue, not MBState.
- **Button, LC II**: the emulated Egret ADB mouse carries the button (zero
  deltas); the ROM's own ADB driver posts the events. Movement that follows a
  button transition waits until the transition has been sent in an ADB packet.
- **Game mode** (`TOUCH_GAME_MODE`, both machines): when a windowless non-Finder
  app is in front (WindowList $09D6 = NULL, scored over ~1 s with hysteresis)
  or the cursor is hidden, clicks are posted with `where` in the menu bar
  (v = 1), which System 7's Process Manager leaves to the front app instead of
  switching to the Finder. MBState is set too (games that poll `_Button`).
