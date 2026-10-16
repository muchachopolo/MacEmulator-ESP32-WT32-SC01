# Hardware: WT32-SC01-3.4

## Board Overview

The WT32-SC01-3.4 is a development board by Wireless Tag featuring an ESP32-WROVER-B module with an integrated 3.5" LCD and capacitive touch panel.

**Datasheets** (in project root):
- `Getting+Started+Guide+for+WT32-SC01.pdf` — Board pinout and setup
- `WT32-SC01+DataSheetV3.4++with+16MB.pdf` — Full board datasheet
- `ST7796s.pdf` — LCD controller datasheet
- `esp32-wrover-b_datasheet_en.pdf` — ESP32-WROVER-B module datasheet

## Actual Board Specifications (verified 2026-04-08)

Board diagnostic flashed and probed over serial. Key finding: **flash is 4MB, not 16MB** as the datasheet/label claims.

| Feature | Datasheet | Actual (measured) |
|---------|-----------|-------------------|
| Chip | ESP32-WROVER-B | **ESP32-D0WD** rev v1.0 |
| CPU | Dual-core Xtensa LX6 @ 240MHz | Confirmed |
| Internal SRAM | 520KB | ~365KB free at boot |
| Flash | 16MB (quad SPI) | **4MB** (chip ID 0x5E4016) |
| PSRAM | 8MB (quad SPI @ 80MHz) | 8MB detected, **~4MB usable** (address space limit) |
| WiFi | 802.11 b/g/n | Confirmed |
| Bluetooth | 4.2 BR/EDR + BLE | Confirmed |
| USB-UART | — | CH340 (driver install required) |

### PSRAM Virtual Address Limitation

The ESP32 can only map 4MB of PSRAM into its virtual address space at a time (0x3F800000–0x3FC00000). The remaining 4MB is accessible only via bank switching (`CONFIG_SPIRAM_BANKSWITCH_ENABLE`), which is enabled but not practical for heap allocation. Effective usable PSRAM for our application is ~4MB.

### Flash Size Discrepancy

The board label and datasheet claim 16MB flash, but esptool and the runtime both detect 4MB. The JEDEC ID `0x5E4016` confirms a 4MB device. This may be a different board revision or batch. The emulator firmware (2.4MB with ROM+disk) fits comfortably in 4MB.

## Display: ST7796 (480x320)

The ST7796 is a 480x320 RGB565 TFT LCD controller connected via SPI.

| Signal | GPIO | Notes |
|--------|------|-------|
| MOSI | 13 | SPI data out |
| SCLK | 14 | SPI clock |
| CS | 15 | Chip select (active low) |
| DC | 21 | Data/Command select |
| RST | 22 | Reset (active low) |
| BL | 23 | Backlight (GPIO on/off) |
| MISO | NC | Not connected (write-only) |

- **SPI host**: SPI2_HOST
- **SPI clock**: 40MHz
- **Color format**: RGB565 (16-bit), BGR element order
- **Orientation**: Landscape (swap XY, mirror Y)
- **DMA**: SPI DMA with auto channel, 10-deep transfer queue
- **Panel driver**: `esp_lcd_new_panel_st7789()` (ST7796 is register-compatible with ST7789)

### DMA Synchronization

The display driver uses a FreeRTOS binary semaphore for DMA synchronization:
1. `display_draw()` starts a DMA transfer via `esp_lcd_panel_draw_bitmap()`
2. The SPI ISR calls `on_color_trans_done()` which gives the semaphore
3. `display_draw()` blocks on `xSemaphoreTake()` until DMA completes

This ensures the caller's buffer isn't modified while DMA is in progress.

## Touch: FT5x06 (Capacitive)

The FT5x06 is a capacitive touch controller connected via I2C.

| Signal | GPIO | Notes |
|--------|------|-------|
| SDA | 18 | I2C data |
| SCL | 19 | I2C clock |

- **I2C address**: 0x38
- **I2C speed**: 400kHz (Fast mode)
- **I2C port**: I2C_NUM_0

### Touch Register Layout

| Register | Content |
|----------|---------|
| 0x02 | Number of touch points (low nibble) |
| 0x03 | Touch X high (bits 11:8 in low nibble) |
| 0x04 | Touch X low (bits 7:0) |
| 0x05 | Touch Y high (bits 11:8 in low nibble) |
| 0x06 | Touch Y low (bits 7:0) |

Touch coordinates are read as a 7-byte block starting at register 0x00. The coordinates may need swapping for landscape orientation.

## PSRAM Considerations

The ESP32-WROVER-B uses VSPI for PSRAM access. Since the display uses SPI2_HOST (HSPI), there is no bus conflict.

PSRAM is configured at 80MHz quad SPI with bank switching (8 virtual banks). Large allocations use `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`. Static BSS can be placed in PSRAM using `EXT_RAM_BSS_ATTR` (requires `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`).

**Important**: PSRAM is slower than internal SRAM (~2-4x latency). Performance-critical data (DMA buffers, ISR data) should stay in internal SRAM.
