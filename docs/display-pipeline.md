# Display Pipeline

## Overview

The Macintosh Classic has a 512x342 monochrome (1-bit) built-in display. The emulator renders this to the WT32-SC01's 480x320 RGB565 (16-bit color) ST7796 LCD via SPI DMA.

## Pipeline Stages

```
Mac RAM (1-bit framebuffer)
         │
         ▼
┌─────────────────────┐
│ Nearest-Neighbor     │  512x342 → 480x320
│ Scale + 1-bit→RGB565 │  Precomputed LUT
│ (mac_video.c)        │  20-line chunks
└─────────┬───────────┘
          │ RGB565 buffer (480 * 20 * 2 = 19,200 bytes)
          ▼
┌─────────────────────┐
│ esp_lcd_panel_draw   │  SPI DMA transfer
│ _bitmap()            │  40MHz SPI clock
│ (display.cpp)        │  Blocking via semaphore
└─────────┬───────────┘
          │ SPI bus
          ▼
┌─────────────────────┐
│ ST7796 LCD Panel     │  480x320 RGB565
│ Hardware             │
└─────────────────────┘
```

## Classic native 480x320 mode (CLASSIC_NATIVE_LCD, default ON)

The Classic build patches the ROM so the Mac itself uses a 480x320 visible
screen (Mini vMac "screen hack" precedent), and the renderer copies 1:1. The
framebuffer layout is unchanged (ScrnBase = MemTop-$5900, rowBytes 64,
512x342 bytes); columns 480-511 and rows 320-341 are unused margin.
Patched immediate words (ROM file offsets, applied in mac_memory.c's ROM read
paths):

| Offset | Routine | Change |
|--------|---------|--------|
| $0060A / $0060E | InitCrsr globals (CrsrPin $0834) | 342/512 -> 320/480 |
| $18F9A / $18FA0 | ScrnSize (JScrnSize $0810) | 342/512 -> 320/480 |
| $18FB4 | _ScrnBitMap (InitGraf -> screenBits.bounds) | 342 -> 320 |

It needs the corrected Classic VIA timer model (`CLASSIC_VIA_INTERP` +
`CLASSIC_VIA_ECLOCK`, mac_via.c); without it a System Time Manager
calibration divides by zero once the boot timing shifts. `CLASSIC_NATIVE_LCD=0`
restores the scaled path below. Strips whose source bytes did not change
are skipped (`MAC_VIDEO_DIRTY`, both builds).

## Scaling Algorithm

The Mac's 512x342 resolution doesn't exactly fit the 480x320 display. We use nearest-neighbor scaling with precomputed lookup tables:

```
X scaling: 480 display pixels ← 512 source pixels (93.75%, slight shrink)
Y scaling: 320 display pixels ← 342 source pixels (93.57%, slight shrink)
```

At init time, two lookup tables are built:

```c
x_src_table[DISPLAY_WIDTH]   // For each display X → source X
y_src_table[DISPLAY_HEIGHT]  // For each display Y → source Y

// Example: x_src_table[240] = (240 * 512) / 480 = 256
```

This avoids per-pixel multiply/divide during rendering.

## 1-Bit to RGB565 Conversion

The Mac framebuffer stores pixels as 1 bit each, packed 8 per byte, MSB first:

```
Byte at offset N:
  Bit 7 = leftmost pixel
  Bit 0 = rightmost pixel
  1 = black (0x0000 in RGB565)
  0 = white (0xFFFF in RGB565)
```

For each display pixel:
```c
int sx = x_src_table[dx];           // Source X from LUT
int byte_idx = sx >> 3;             // Which byte (sx / 8)
int bit_idx = 7 - (sx & 7);        // Which bit (MSB first)
int pixel = (src_line[byte_idx] >> bit_idx) & 1;
*out++ = pixel ? 0x0000 : 0xFFFF;  // Black or white
```

## Chunked Rendering

To limit memory usage, the display is rendered in chunks of 20 lines:

- **Line buffer**: 480 * 20 * 2 bytes = 19,200 bytes in DMA-capable internal SRAM
- **16 chunks** per frame (320 / 20 = 16)
- Each chunk: convert → DMA → wait for completion → next chunk

The line buffer is allocated with `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` because ESP32 SPI DMA can only access internal SRAM, not PSRAM.

## Timing Budget

At 60fps, each frame has ~16.6ms:
- 16 chunks * SPI transfer time per chunk
- 480 * 20 pixels * 16 bits / 40MHz SPI = ~3.84ms per chunk
- But with 10-deep SPI queue and DMA, CPU is free during transfer
- Total SPI time: ~3.84ms * 16 = ~61ms (too slow for 60fps)

**Reality**: The display rendering is the bottleneck. Actual frame rate will be lower than 60fps. Potential optimizations:
- Dirty-rectangle tracking (only redraw changed regions)
- Larger DMA buffer (trade memory for fewer transfers)
- Double buffering with PSRAM-backed buffer
- Reduce SPI overhead by sending full lines instead of chunks

## Touch Input

Touch coordinates from the FT5x06 (0-479, 0-319) are scaled to Mac coordinates (0-511, 0-341):

```c
mac_x = (touch_x * 512) / 480;
mac_y = (touch_y * 342) / 320;
```

Touch → mouse button down. Release → mouse button up.
