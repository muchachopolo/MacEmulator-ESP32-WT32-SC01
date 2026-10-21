/*
 * Mac Video Renderer
 *
 * Converts the Mac's 512x342 1-bit framebuffer to 480x320 RGB565
 * and pushes it to the display via SPI DMA.
 *
 * Rendering strategy:
 *   - Precompute byte lookup table: each framebuffer byte -> 8 RGB565 pixels
 *   - Classic native mode copies 480x320 1:1; the scaled path (stock
 *     CLASSIC_NATIVE_LCD=0, or the game full view) drops 1 pixel in 16
 *     horizontally and samples rows through y_src_table
 *   - Convert and send CHUNK_LINES lines at a time to limit memory usage
 *   - Runs on a separate FreeRTOS task (core 0) to decouple from CPU emulation
 */
#include "mac_video.h"
#include "mac_emulator.h"
#include "mac_memory.h"
#include "display.h"
#if MAC_ROM_LC_II
#include "mac_v8video.h"
#endif

#include <string.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_cpu.h>
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

/* Y scaling: source line for each display line */
static uint16_t y_src_table[DISPLAY_HEIGHT];

/* Line buffer for one chunk of RGB565 output (in DMA-capable SRAM) */
static uint16_t *line_buf = NULL;

/* Dirty-chunk skipping (MAC_VIDEO_DIRTY, default 1): each 20-line strip's
 * source bytes are hashed (word-wise, ~1 PSRAM read of the visible frame per
 * render, which the conversion would read anyway) and the strip is only
 * converted + sent over SPI when its hash changed, or on a forced redraw
 * (first frame, depth/size/CLUT/base change). Nothing is added to the CPU
 * emulation's write path. An idle Finder then costs core 0 one framebuffer
 * read per frame instead of a full LUT conversion + 300KB of SPI DMA, and
 * mac_video_task sleeps a tick when nothing changed, leaving PSRAM/cache
 * bandwidth to core 1. 0 = always redraw everything (old behaviour). */
#ifndef MAC_VIDEO_DIRTY
#define MAC_VIDEO_DIRTY 1
#endif
#define NUM_CHUNKS ((DISPLAY_HEIGHT + CHUNK_LINES - 1) / CHUNK_LINES)
#if MAC_VIDEO_DIRTY
static uint32_t chunk_hash[NUM_CHUNKS];
static bool     chunk_force = true;
static uint32_t chunk_key;              /* base/depth/geometry/CLUT gen */
static int      chunks_sent;            /* last render (0 = idle frame) */

/* FNV-style hash over n bytes of a framebuffer row (word reads when aligned) */
static inline uint32_t row_hash(uint32_t h, const uint8_t *p, uint32_t n)
{
    if (((uintptr_t)p & 3) == 0) {
        const uint32_t *w = (const uint32_t *)p;
        for (uint32_t i = 0; i < n / 4; i++) h = (h ^ w[i]) * 16777619u;
        p += n & ~3u; n &= 3;
    }
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
/* Returns true if the chunk must be drawn (and records its new hash). */
static inline bool chunk_dirty(int ci, uint32_t h)
{
    if (!chunk_force && chunk_hash[ci] == h) return false;
    chunk_hash[ci] = h;
    chunks_sent++;
    return true;
}
static inline void chunk_set_key(uint32_t key)
{
    if (key != chunk_key) { chunk_key = key; chunk_force = true; }
}
#endif

volatile bool mac_full_view;

static uint32_t g_scrn_base;
static uint32_t g_fb_xor;
static uint8_t g_fb0, g_fb1;
static char g_fbdump[49];

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

    /* X scaling (512 -> 480, sx = dx*16/15) is folded into the render loop:
     * 15 display pixels per 2 source bytes. */

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

#if MAC_ROM_LC_II
/* ===================== V8 (LC II / Color Classic) path =====================
 * Source: VRAM ($F40000, PSRAM buffer owned by mac_v8video.c), depth from
 * MonP bits 0-2 (0..3 = 1/2/4/8 bpp, 4 = 16bpp xRGB1555), stride from
 * ScreenRow ($0106; V8 uses a fixed 1024), colours through the VDAC CLUT.
 *
 * As in MAME v8/rbv, an n-bit pixel p indexes the CLUT at
 * (p << (8-n)) | (0xFF >> n): 1bpp uses entries $7F/$FF, 2bpp $3F/$7F/..
 *
 * All depths <= 8 share one 4KB LUT: byte -> up to 8 RGB565 pixels, rebuilt
 * only when the depth or CLUT generation changes. The per-pixel loop is the
 * same 2-table + LUT lookup as the Classic path (no per-pixel branches). */
static uint16_t v8_lut[256][8];
static uint16_t v8_xb[DISPLAY_WIDTH];   /* source byte offset within row */
static uint8_t  v8_xs[DISPLAY_WIDTH];   /* pixel index within that byte  */
static uint16_t v8_ys[DISPLAY_HEIGHT];  /* source row */
static int      v8_lut_depth = -1;
static uint32_t v8_lut_gen;
static int      v8_tab_depth = -1;
static int      v8_tab_w, v8_tab_h;
static uint32_t mac_video_v8_cycles;    /* last render's CPU cycles (diag) */

static void v8_build_lut(int d, const uint16_t *pal)
{
    if (d > 3) d = 3;
    int bpp = 1 << d, ppb = 8 >> d, mask = (1 << bpp) - 1;
    for (int b = 0; b < 256; b++) {
        for (int k = 0; k < ppb; k++) {
            int p = (b >> (8 - bpp * (k + 1))) & mask;
            v8_lut[b][k] = pal[((p << (8 - bpp)) | (0xFF >> bpp)) & 0xFF];
        }
    }
}

static void v8_build_tables(int d, int sw, int sh)
{
    int ppb_shift = (d >= 3) ? 0 : 3 - d;           /* log2(pixels per byte) */
    for (int dx = 0; dx < DISPLAY_WIDTH; dx++) {
        int sx = (dx * sw) / DISPLAY_WIDTH;
        if (d >= 4) { v8_xb[dx] = (uint16_t)(sx * 2); v8_xs[dx] = 0; }
        else {
            v8_xb[dx] = (uint16_t)(sx >> ppb_shift);
            v8_xs[dx] = (uint8_t)(sx & ((1 << ppb_shift) - 1));
        }
    }
    for (int dy = 0; dy < DISPLAY_HEIGHT; dy++)
        v8_ys[dy] = (uint16_t)((dy * sh) / DISPLAY_HEIGHT);
    v8_tab_depth = d; v8_tab_w = sw; v8_tab_h = sh;
}

static inline uint16_t v8_px16(const uint8_t *p)
{
    /* xRGB1555 big-endian -> RGB565 (G gets a replicated low bit) */
    uint16_t w = (uint16_t)((p[0] << 8) | p[1]);
    uint16_t v = (uint16_t)(((w & 0x7C00) << 1) | ((w & 0x03E0) << 1) |
                            ((w >> 4) & 0x20) | (w & 0x1F));
#ifndef HOST_BUILD
    v = (uint16_t)((v >> 8) | (v << 8));
#endif
    return v;
}

static void v8_render(const uint8_t *mac_ram)
{
    const uint8_t *vram = v8video_get_vram();
    if (!vram) return;
    uint32_t t0 = esp_cpu_get_cycle_count();

    int d = v8video_depth_log2();
    if (d > 4) d = 3;
    const uint16_t *pal = v8video_get_pal565();
    uint32_t gen = v8video_get_pal_gen();
    if (d <= 3 && (d != v8_lut_depth || gen != v8_lut_gen)) {
        v8_lut_gen = gen;
        v8_build_lut(d, pal);
        v8_lut_depth = d;
    }
    int sw = v8video_screen_width(), sh = v8video_screen_height();
    if (d != v8_tab_depth || sw != v8_tab_w || sh != v8_tab_h)
        v8_build_tables(d, sw, sh);

    /* Frame base: ScrnBase ($0824) inside the VRAM window, else offset 0.
     * Stride: ScreenRow ($0106) when sane, else the V8 fixed 1024. */
    uint32_t sb = (((uint32_t)mac_ram[0x824] << 24) | ((uint32_t)mac_ram[0x825] << 16) |
                   ((uint32_t)mac_ram[0x826] << 8) | mac_ram[0x827]) & 0xFFFFFF;
    uint32_t base = (sb >= V8_VRAM_BASE && sb < V8_VRAM_BASE + V8_VRAM_SIZE)
                    ? sb - V8_VRAM_BASE : 0;
    uint32_t stride = ((uint32_t)mac_ram[0x106] << 8) | mac_ram[0x107];
    stride &= 0x3FFF;
    if (stride < 64 || stride > 2048) stride = 1024;
    uint32_t need = (uint32_t)(sh - 1) * stride + (uint32_t)(sw << d) / 8 + 1;
    if (base + need > V8_VRAM_SIZE) base = 0;
    const uint8_t *screen = vram + base;
    g_scrn_base = sb;
    g_fb0 = screen[0];
    g_fb1 = screen[1];

    bool one_to_one = (sw == DISPLAY_WIDTH && sh == DISPLAY_HEIGHT);
    int ppb = 8 >> (d > 3 ? 3 : d);

#if MAC_VIDEO_DIRTY
    chunk_set_key((uint32_t)(uintptr_t)screen ^ ((uint32_t)d << 28) ^ (stride << 16) ^
                  ((uint32_t)sw << 4) ^ ((uint32_t)sh << 20) ^ (gen * 2654435761u));
    chunks_sent = 0;
    uint32_t row_bytes = (d >= 4) ? (uint32_t)sw * 2 : ((uint32_t)sw << d) / 8;
#endif
    for (int chunk_start = 0; chunk_start < DISPLAY_HEIGHT; chunk_start += CHUNK_LINES) {
        int chunk_end = chunk_start + CHUNK_LINES;
        if (chunk_end > DISPLAY_HEIGHT) chunk_end = DISPLAY_HEIGHT;
        uint16_t *out = line_buf;
#if MAC_VIDEO_DIRTY
        {
            uint32_t h = 2166136261u;
            for (int dy = chunk_start; dy < chunk_end; dy++)
                if (dy == chunk_start || v8_ys[dy] != v8_ys[dy - 1])
                    h = row_hash(h, screen + (uint32_t)v8_ys[dy] * stride, row_bytes);
            if (!chunk_dirty(chunk_start / CHUNK_LINES, h)) continue;
        }
#endif

        for (int dy = chunk_start; dy < chunk_end; dy++) {
            const uint8_t *src = screen + (uint32_t)v8_ys[dy] * stride;
            if (d >= 4) {
                for (int dx = 0; dx < DISPLAY_WIDTH; dx++)
                    *out++ = v8_px16(src + v8_xb[dx]);
            } else if (one_to_one) {
                int nbytes = DISPLAY_WIDTH / ppb;
                if (d == 3) {
                    for (int i = 0; i < nbytes; i++) *out++ = pal[src[i]];
                } else if (d == 0) {
                    for (int i = 0; i < nbytes; i++) {
                        const uint16_t *l = v8_lut[src[i]];
                        out[0] = l[0]; out[1] = l[1]; out[2] = l[2]; out[3] = l[3];
                        out[4] = l[4]; out[5] = l[5]; out[6] = l[6]; out[7] = l[7];
                        out += 8;
                    }
                } else {
                    for (int i = 0; i < nbytes; i++) {
                        memcpy(out, v8_lut[src[i]], ppb * sizeof(uint16_t));
                        out += ppb;
                    }
                }
            } else {
                for (int dx = 0; dx < DISPLAY_WIDTH; dx++)
                    *out++ = v8_lut[src[v8_xb[dx]]][v8_xs[dx]];
            }
        }
        display_draw(0, chunk_start, DISPLAY_WIDTH, chunk_end - chunk_start, line_buf);
    }
#if MAC_VIDEO_DIRTY
    chunk_force = false;
#endif
    mac_video_v8_cycles = esp_cpu_get_cycle_count() - t0;
}
#endif /* MAC_ROM_LC_II */

void mac_video_render(const uint8_t *mac_ram, uint32_t ram_size)
{
    if (!line_buf || !mac_ram) return;
#if MAC_ROM_LC_II
    v8_render(mac_ram);
    return;
#endif

    /* Read ScrnBase from low-memory global $0824 (set by ROM sizing).
     * This is dynamic — the ROM sets it based on detected RAM size. */
    uint32_t scrn_base = ((uint32_t)mac_ram[0x824] << 24) |
                         ((uint32_t)mac_ram[0x825] << 16) |
                         ((uint32_t)mac_ram[0x826] << 8) |
                         mac_ram[0x827];
    if (scrn_base == 0) {
        scrn_base = ram_size - 0x5900;  /* fallback */
    } else if (scrn_base >= ram_size) {
        scrn_base &= (ram_size - 1);
    }
    const uint8_t *screen = mac_ram + scrn_base;
    g_scrn_base = scrn_base;
    g_fb0 = screen[0];
    g_fb1 = screen[1];
    uint32_t fb_xor = 0;
    for (uint32_t i = 0; i < 0x5900; i += 64)
        fb_xor ^= (screen[i] << 8) | screen[i + 1];
    g_fb_xor = fb_xor;
    {
        static const char hx[] = "0123456789ABCDEF";
        const uint8_t *d = screen + 0x200;
        int o = 0;
        for (int bi = 0; bi < 16; bi++) {
            g_fbdump[o++] = hx[d[bi] >> 4];
            g_fbdump[o++] = hx[d[bi] & 15];
            g_fbdump[o++] = ' ';
        }
        g_fbdump[o] = 0;
    }

    /* Scaled 512x342 -> 480x320 (stock CLASSIC_NATIVE_LCD=0, or the game
     * full view) vs 1:1 top-left 480x320. Read once per render. */
#if CLASSIC_NATIVE_LCD
    const bool scaled = mac_full_view;
#else
    const bool scaled = true;
#endif
#if MAC_VIDEO_DIRTY
    chunk_set_key(scrn_base ^ (scaled ? 0x80000000u : 0));
    chunks_sent = 0;
#endif
    /* Process display in chunks of CHUNK_LINES */
    for (int chunk_start = 0; chunk_start < DISPLAY_HEIGHT; chunk_start += CHUNK_LINES) {
        int chunk_end = chunk_start + CHUNK_LINES;
        if (chunk_end > DISPLAY_HEIGHT) chunk_end = DISPLAY_HEIGHT;
        int chunk_height = chunk_end - chunk_start;
#if MAC_VIDEO_DIRTY
        {
            uint32_t h = 2166136261u;
            if (!scaled) {
                for (int dy = chunk_start; dy < chunk_end; dy++)
                    h = row_hash(h, screen + dy * MAC_SCREEN_STRIDE, DISPLAY_WIDTH / 8);
            } else {
                /* scaled: hash every source line this strip samples */
                int y0 = y_src_table[chunk_start], y1 = y_src_table[chunk_end - 1];
                h = row_hash(h, screen + y0 * MAC_SCREEN_STRIDE, (y1 - y0 + 1) * MAC_SCREEN_STRIDE);
            }
            if (!chunk_dirty(chunk_start / CHUNK_LINES, h)) continue;
        }
#endif

        /* Convert each line in this chunk using precomputed LUT */
        uint16_t *out = line_buf;

        for (int dy = chunk_start; dy < chunk_end; dy++) {
            if (!scaled) {
                /* Native 480x320: the Mac's visible screen is the top-left
                 * 480x320 of the 512x342 buffer, copied 1:1 (60 bytes/row). */
                const uint8_t *src_line = screen + dy * MAC_SCREEN_STRIDE;
                for (int i = 0; i < DISPLAY_WIDTH / 8; i++) {
                    const uint16_t *l = byte_lut[src_line[i]];
                    out[0] = l[0]; out[1] = l[1]; out[2] = l[2]; out[3] = l[3];
                    out[4] = l[4]; out[5] = l[5]; out[6] = l[6]; out[7] = l[7];
                    out += 8;
                }
            } else {
                /* Nearest-neighbour 512 -> 480: display x = 15k+j samples
                 * source x = 16k+j (j < 15), i.e. each 16-pixel source pair
                 * of bytes gives 15 pixels, dropping the last pixel (bit 0)
                 * of the second byte. Same as sx = dx*512/480 per
                 * pixel, at the cost of the 1:1 byte-LUT copy. */
                const uint8_t *src_line = screen + y_src_table[dy] * MAC_SCREEN_STRIDE;
                for (int i = 0; i < MAC_SCREEN_STRIDE; i += 2) {
                    const uint16_t *a = byte_lut[src_line[i]];
                    const uint16_t *b = byte_lut[src_line[i + 1]];
                    out[0] = a[0]; out[1] = a[1]; out[2] = a[2]; out[3] = a[3];
                    out[4] = a[4]; out[5] = a[5]; out[6] = a[6]; out[7] = a[7];
                    out[8] = b[0]; out[9] = b[1]; out[10] = b[2]; out[11] = b[3];
                    out[12] = b[4]; out[13] = b[5]; out[14] = b[6];
                    out += 15;
                }
            }
        }

        /* Send this chunk to the display */
        display_draw(0, chunk_start, DISPLAY_WIDTH, chunk_height, line_buf);
    }
#if MAC_VIDEO_DIRTY
    chunk_force = false;
#endif
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
#if MAC_VIDEO_DIRTY
        if (chunks_sent) fps_count++;   /* frames that changed the LCD */
#else
        fps_count++;
#endif

        /* Log video FPS every 10 seconds */
        int64_t now = esp_timer_get_time();
        if (now - last_fps_log > 10000000) {
            ESP_LOGI(TAG, "Video: %d fps scrn=%06X fb=%02X%02X xor=%08X bar=%s",
                     fps_count * 1000000 / (int)(now - last_fps_log),
                     (unsigned)g_scrn_base, g_fb0, g_fb1, (unsigned)g_fb_xor, g_fbdump);
#if MAC_ROM_LC_II
            ESP_LOGI(TAG, "V8 render: %u cycles (%dx%d d=%d)", (unsigned)mac_video_v8_cycles,
                     v8video_screen_width(), v8video_screen_height(), v8video_depth_log2());
#endif
            fps_count = 0;
            last_fps_log = now;
        }

#if MAC_VIDEO_DIRTY
        /* Nothing changed: sleep a tick instead of re-reading the
         * framebuffer at full speed (PSRAM bandwidth belongs to core 1). */
        if (chunks_sent == 0) {
            vTaskDelay(1);
            continue;
        }
#endif
        /* Yield to let other core-0 tasks run (WiFi, etc.) */
        taskYIELD();
    }
}
