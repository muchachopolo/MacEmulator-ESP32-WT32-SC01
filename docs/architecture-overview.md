# Architecture Overview

## System Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                    ESP32-WROVER-B (WT32-SC01-3.4)           │
│                                                             │
│  Core 0 (FreeRTOS)           Core 1 (Emulator Task)        │
│  ┌──────────────┐            ┌──────────────────────┐       │
│  │  app_main()  │            │  mac_emu_run()       │       │
│  │  Display init│───start───>│                      │       │
│  │  Emu init    │            │  ┌────────────────┐  │       │
│  │  Idle loop   │            │  │ Musashi 68000  │  │       │
│  └──────────────┘            │  │ CPU Emulator   │  │       │
│                              │  └───────┬────────┘  │       │
│                              │          │           │       │
│                              │  ┌───────▼────────┐  │       │
│                              │  │ Memory Bus     │  │       │
│                              │  │ (mac_memory.c) │  │       │
│                              │  └──┬──┬──┬──┬──┬─┘  │       │
│                              │     │  │  │  │  │    │       │
│                              │  ┌──▼──▼──▼──▼──▼─┐  │       │
│                              │  │RAM│ROM│VIA│SCC│IWM│ │       │
│                              │  └──┬────────────┘  │       │
│                              │     │               │       │
│                              │  ┌──▼─────────────┐ │       │
│                              │  │ Video Render   │ │       │
│                              │  │ (mac_video.c)  │ │       │
│                              │  └──────┬─────────┘ │       │
│                              └─────────┼───────────┘       │
│                                        │                   │
│  ┌─────────────────────────────────────▼─────────┐         │
│  │            Display Driver (display.cpp)        │         │
│  │         SPI DMA → ST7796 480x320 LCD           │         │
│  │         I2C → FT5x06 Touch Controller          │         │
│  └────────────────────────────────────────────────┘         │
│                                                             │
│  ┌──────────────────────────┐  ┌────────────────────────┐   │
│  │ Internal SRAM (~365KB)   │  │ PSRAM (8MB, 4MB usable)│   │
│  │  - FreeRTOS stacks       │  │  - Mac RAM (2MB)       │   │
│  │  - DMA line buffer       │  │  - Disk image (1.44MB) │   │
│  │  - ESP-IDF runtime       │  │  - Musashi tables      │   │
│  │  - ~302KB DMA-capable    │  │    (580KB, BSS)        │   │
│  └──────────────────────────┘  └────────────────────────┘   │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐   │
│  │ Flash (4MB — actual, not 16MB as labeled)            │   │
│  │  - Bootloader + partition table                      │   │
│  │  - Application firmware (~2.4MB)                     │   │
│  │  - Embedded Classic.ROM (512KB, .rodata)             │   │
│  │  - Embedded OS_750_boot.dsk (1.44MB, .rodata)        │   │
│  │  - 1.7MB free in partition                           │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

## Emulation Loop

The emulator runs a frame-based loop at ~60Hz on Core 1:

```
Each frame (~16.6ms):
  1. Execute ~130,560 CPU cycles (7.83MHz / 60Hz)
     - Run in slices of 1000 cycles
     - After each slice: tick VIA timers, check/raise IRQs
  2. Trigger VBlank interrupt (VIA CA1)
  3. Render framebuffer to display
     - Read 1-bit pixels from Mac RAM
     - Convert 512x342 → 480x320 RGB565 (nearest-neighbor)
     - Send via SPI DMA in 20-line chunks
  4. Poll touch input → update mouse position
  5. Frame-rate limit (sleep if ahead of schedule)
```

## Source File Map

| File | Purpose | Lines |
|------|---------|-------|
| `main/main.cpp` | Entry point, display init, launches emu task on Core 1 | ~67 |
| `main/mac_emulator.c` | Frame loop, ROM/disk loading, touch→mouse | ~169 |
| `main/mac_memory.c` | 24-bit address bus decode, Musashi read/write callbacks | ~252 |
| `main/mac_video.c` | 1-bit→RGB565 conversion, chunked DMA rendering | ~101 |
| `main/mac_via.c` | VIA 6522: timers, interrupts, overlay control | ~260 |
| `main/mac_scc.c` | SCC stub (returns safe defaults) | ~48 |
| `main/mac_iwm.c` | IWM floppy controller (sense lines, basic status) | ~143 |
| `components/musashi/` | Musashi 68000 CPU core (third-party, copied) | ~37K |
| `components/display/display.cpp` | ST7796 SPI + FT5x06 I2C driver | ~187 |

## Component Dependencies

```
main
├── display (esp_lcd, SPI, I2C, GPIO)
├── musashi (standalone, log)
├── freertos
├── esp_timer
└── esp_psram

display
├── esp_lcd (panel IO, panel ops)
├── esp_driver_spi
├── esp_driver_gpio
├── esp_driver_i2c
└── freertos (semaphore for DMA sync)

musashi
└── log (ESP_LOG only)
```

## Memory Budget (verified on hardware)

| Resource | Used | Available | Notes |
|----------|------|-----------|-------|
| DRAM (.data+.bss) | 16KB | 180KB | Minimal after moving tables to PSRAM |
| IRAM (.text) | 60KB | 131KB | ISR handlers, cache workarounds |
| Flash (.text+.rodata) | 2.4MB | 3.9MB partition | Includes embedded ROM (512KB) + disk (1.44MB) |
| PSRAM (static BSS) | 580KB | — | Musashi opcode/cycle tables |
| PSRAM (heap at boot) | ~3.4MB | ~4MB usable | 2MB Mac RAM + 1.44MB disk image (writable copy) |
| App partition | 2.4MB | 3.9MB | 41% free (1.7MB headroom) |
| Internal SRAM (free) | — | ~365KB | 302KB DMA-capable |

### Disk Image Strategy

The 1.44MB disk image is embedded in flash (.rodata) but **copied to PSRAM at boot**. This serves two purposes:
1. The flash copy is read-only; the PSRAM copy is writable (needed for disk writes/OS installation)
2. RAM-resident disk is faster to access than flash for random reads

Future: A virtual SCSI hard drive could use remaining PSRAM or a dedicated flash partition as a persistent disk target for OS installation.
