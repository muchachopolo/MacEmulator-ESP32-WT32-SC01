/*
 * Mac Video Renderer
 *
 * Converts the Mac's 512x342 1-bit framebuffer to 480x320 RGB565
 * and pushes it to the display via SPI DMA.
 *
 * Rendering strategy:
 *   - Precompute byte lookup table: each framebuffer byte -> 8 RGB565 pixels
 *   - Precompute X mapping tables for nearest-neighbor scaling
 *   - Convert and send CHUNK_LINES lines at a time to limit memory usage
 *   - Runs on a separate FreeRTOS task (core 0) to decouple from CPU emulation
 */
#include "mac_video.h"
#include "mac_emulator.h"
#include "mac_memory.h"
#include "display.h"

#include <string.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "mac_video";

/* How many display lines to convert and send at once */
#define CHUNK_LINES     20

/* RGB565 colors */
#define COLOR_WHITE     0xFFFF
#define COLOR_BLACK     0x0000

/* Byte lookup table: for each possible framebuffer byte (0-255),
 * the 8 corresponding RGB565 pixel values.
 * byte_lut[b][0] = leftmost pixel (bit 7), byte_lut[b][7] = rightmost (bit 0).
 * Mac convention: 1 = black, 0 = white. */
static uint16_t byte_lut[256][8];

/* Precomputed X mapping: for each display X (0-479), the source byte index
 * and bit position within that byte. Avoids per-pixel division/shift. */
static uint16_t x_byte_table[DISPLAY_WIDTH];
static uint8_t  x_bit_table[DISPLAY_WIDTH];

/* Y scaling: source line for each display line */
static uint16_t y_src_table[DISPLAY_HEIGHT];

/* Line buffer for one chunk of RGB565 output (in DMA-capable SRAM) */
static uint16_t *line_buf = NULL;

bool mac_video_init(void)
{
    /* Build byte LUT: maps each framebuffer byte to 8 RGB565 pixels */
    for (int b = 0; b < 256; b++) {
        for (int bit = 0; bit < 8; bit++) {
            /* bit index 0 = MSB (leftmost pixel), 7 = LSB (rightmost) */
            int pixel = (b >> (7 - bit)) & 1;
            byte_lut[b][bit] = pixel ? COLOR_BLACK : COLOR_WHITE;
        }
    }

    /* Precompute X mapping: 480 display pixels from 512 source pixels */
    for (int dx = 0; dx < DISPLAY_WIDTH; dx++) {
        int sx = (dx * MAC_SCREEN_WIDTH) / DISPLAY_WIDTH;
        x_byte_table[dx] = sx >> 3;
        x_bit_table[dx]  = sx & 7;
    }

    /* Precompute Y scaling: 320 display pixels from 342 source pixels */
    for (int dy = 0; dy < DISPLAY_HEIGHT; dy++) {
        y_src_table[dy] = (dy * MAC_SCREEN_HEIGHT) / DISPLAY_HEIGHT;
    }

    /* Allocate line buffer in DMA-capable internal SRAM */
    size_t buf_size = DISPLAY_WIDTH * CHUNK_LINES * sizeof(uint16_t);
    line_buf = (uint16_t *)heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!line_buf) {
        /* Fallback to any internal memory */
        line_buf = (uint16_t *)heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL);
    }
    if (!line_buf) {
        ESP_LOGE(TAG, "Failed to allocate %d byte line buffer", (int)buf_size);
        return false;
    }

    ESP_LOGI(TAG, "Video initialized: %dx%d -> %dx%d, chunk=%d lines, buf=%d bytes",
             MAC_SCREEN_WIDTH, MAC_SCREEN_HEIGHT,
             DISPLAY_WIDTH, DISPLAY_HEIGHT,
             CHUNK_LINES, (int)buf_size);
    return true;
}

void mac_video_render(const uint8_t *mac_ram, uint32_t ram_size)
{
    if (!line_buf || !mac_ram) return;

    /* Read ScrnBase from low-memory global $0824 (set by ROM sizing).
     * This is dynamic — the ROM sets it based on detected RAM size. */
    uint32_t scrn_base = ((uint32_t)mac_ram[0x824] << 24) |
                         ((uint32_t)mac_ram[0x825] << 16) |
                         ((uint32_t)mac_ram[0x826] << 8) |
                         mac_ram[0x827];
    if (scrn_base == 0 || scrn_base >= ram_size) {
        scrn_base = ram_size - 0x5900;  /* fallback */
    }
    const uint8_t *screen = mac_ram + scrn_base;

    /* Process display in chunks of CHUNK_LINES */
    for (int chunk_start = 0; chunk_start < DISPLAY_HEIGHT; chunk_start += CHUNK_LINES) {
        int chunk_end = chunk_start + CHUNK_LINES;
        if (chunk_end > DISPLAY_HEIGHT) chunk_end = DISPLAY_HEIGHT;
        int chunk_height = chunk_end - chunk_start;

        /* Convert each line in this chunk using precomputed LUT */
        uint16_t *out = line_buf;

        for (int dy = chunk_start; dy < chunk_end; dy++) {
            const uint8_t *src_line = screen + y_src_table[dy] * MAC_SCREEN_STRIDE;

            for (int dx = 0; dx < DISPLAY_WIDTH; dx++) {
                *out++ = byte_lut[src_line[x_byte_table[dx]]][x_bit_table[dx]];
            }
        }

        /* Send this chunk to the display */
        display_draw(0, chunk_start, DISPLAY_WIDTH, chunk_height, line_buf);
    }
}

void mac_video_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "Video task started on core %d", xPortGetCoreID());

    /* Wait for emulator to start before rendering */
    vTaskDelay(pdMS_TO_TICKS(500));

    int64_t last_fps_log = esp_timer_get_time();
    int fps_count = 0;

    while (1) {
        uint8_t *ram = mac_mem_get_ram();
        uint32_t ram_size = mac_mem_get_ram_size();
        if (ram && ram_size > 0) {
            mac_video_render(ram, ram_size);
        }
        fps_count++;

        /* Log video FPS every 10 seconds */
        int64_t now = esp_timer_get_time();
        if (now - last_fps_log > 10000000) {
            ESP_LOGI(TAG, "Video: %d fps", fps_count * 1000000 / (int)(now - last_fps_log));
            fps_count = 0;
            last_fps_log = now;
        }

        /* Yield to let other core-0 tasks run (WiFi, etc.) */
        taskYIELD();
    }
}
