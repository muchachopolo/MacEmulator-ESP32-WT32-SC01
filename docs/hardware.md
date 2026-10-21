# Hardware: WT32-SC01 v3.4

The WT32-SC01 v3.4 by Wireless Tag is an ESP32-WROVER-B board with a 3.5"
480×320 SPI LCD and a capacitive touch panel. The emulator needs nothing
else. Other WT32-SC01 variants (for example the ESP32-S3 "Plus" with a
parallel LCD) are **not** supported.

## Datasheets

The vendor PDFs are not stored in this repository; get them from the vendors:

| Document | Vendor | Link |
|---|---|---|
| WT32-SC01 datasheet v3.4, *Getting Started Guide for WT32-SC01* | Wireless Tag | from Wireless Tag or your board seller |
| ESP32-WROVER-B datasheet | Espressif | [esp32-wrover-b_datasheet_en.pdf](https://www.espressif.com/sites/default/files/documentation/esp32-wrover-b_datasheet_en.pdf) |
| ESP32 Technical Reference Manual | Espressif | [esp32_technical_reference_manual_en.pdf](https://www.espressif.com/sites/default/files/documentation/esp32_technical_reference_manual_en.pdf) |
| ESP-IDF Programming Guide (ESP32) | Espressif | [docs.espressif.com](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/) |
| ST7796S LCD controller datasheet | Sitronix | Sitronix (search "ST7796S datasheet") |
| FT5x06 capacitive touch controller datasheet / application note | FocalTech | FocalTech (search "FT5x06 datasheet") |

## Measured specifications (board diagnostic, `board_test/`, 2026-04-08)

| Feature | Datasheet | Measured |
|---|---|---|
| Chip | ESP32-WROVER-B | ESP32-D0WD rev v1.0 |
| CPU | Dual-core Xtensa LX6, 240 MHz | confirmed |
| Flash | 16 MB | **4 MB** (JEDEC ID 0x5E4016) |
| PSRAM | 8 MB quad SPI, 80 MHz | 8 MB, **~4 MB addressable** |
| Internal SRAM | 520 KB | ~365 KB free at boot, ~302 KB DMA-capable |
| USB-UART | — | CH340 (install its driver; the port was COM3 on the dev PC) |

**Flash:** the label and datasheet say 16 MB, but esptool and the runtime both
detect 4 MB. The firmware (code + ROM + disk) must fit the 0x3F0000 app
partition. Flash runs at 80 MHz QIO (+1.1% emulation speed vs DIO).

**PSRAM:** the ESP32 maps only 4 MB of PSRAM into its address space
(0x3F800000–0x3FC00000); the rest would need bank switching, which is not
used. PSRAM is several times slower than internal RAM and is accessed through
a 32 KB cache shared with flash, which is why hot code goes to IRAM.
The chip is rev 1.0, so the PSRAM cache workaround cannot be disabled; MEMW is
used (DUPLDST measured 1.3% slower).

## Display: ST7796S

| Signal | GPIO |
|---|---|
| MOSI | 13 |
| SCLK | 14 |
| CS | 15 |
| DC | 21 |
| RST | 22 |
| Backlight | 23 |
| MISO | not connected |

- SPI2_HOST at 40 MHz, DMA (auto channel), 10-deep transaction queue.
- RGB565, BGR element order; landscape via swap-XY and mirror X/Y.
- Driven with `esp_lcd_new_panel_st7789()` (compatible command set). The native
  esp_lcd driver replaced the `esp32_display_panel` component, which does not
  support ESP-IDF v6.
- `display_draw()` blocks on a semaphore given by the SPI done callback, so the
  caller's buffer is free when it returns.

## Touch: FT5x06

| Signal | GPIO |
|---|---|
| SDA | 18 |
| SCL | 19 |

- I2C_NUM_0, 400 kHz, address 0x38.
- `display_get_touch()` reads 7 bytes from register 0x00: 0x02 = number of
  points (low nibble), 0x03/0x04 = X (12 bits), 0x05/0x06 = Y (12 bits).
- The emulator swaps the axes and inverts the horizontal axis to match the
  landscape display. Gesture handling: [display-pipeline](display-pipeline.md#touch-input).

## Expansion

The board's expansion header is unused today. Planned: an external SPI/I2C
keyboard and a joystick (see [`spec/next-steps-plan.md`](../spec/next-steps-plan.md)).
The emulator has no audio output yet.
