/*
 * V8 (Color Classic / LC II) built-in video: VRAM, VDAC (CLUT) and the
 * RBV-style pseudo-VIA2.  Register behaviour follows MAME's v8.cpp.
 * Only built when MAC_ROM_LC_II is set; the Classic build never calls it.
 */
#include "mac_v8video.h"
#include "mac_via.h"

#if MAC_ROM_LC_II

#include <string.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

static const char *TAG = "v8vid";

/* Apple monitor sense code of the Color Classic built-in 512x384 display */
#define V8_MONITOR_SENSE  2

static uint8_t *vram;
static uint8_t  pvia[0x20];        /* pseudo-VIA2 registers $00-$1F */
static bool     asc_level;         /* ASC interrupt line: IFR bit 4 follows it */
static uint8_t  clut[256 * 3];
static uint8_t  vdac_regs[16];
static uint8_t  vdac_index;
static uint8_t  vdac_comp;         /* 0=R 1=G 2=B */

/* RGB565 copy of the CLUT for the LCD renderer, in the byte order the
 * LCD wants (byte-swapped on the ESP32 because esp_lcd sends the uint16
 * little-endian; native on the host harness). pal_gen bumps on every
 * completed triplet so the renderer can rebuild its byte LUT lazily. */
static uint16_t pal565[256];
static volatile uint32_t pal_gen;

static inline void pal_update(uint8_t idx)
{
    const uint8_t *c = &clut[idx * 3];
    uint16_t v = (uint16_t)(((c[0] & 0xF8) << 8) | ((c[1] & 0xFC) << 3) | (c[2] >> 3));
#ifndef HOST_BUILD
    v = (uint16_t)((v >> 8) | (v << 8));
#endif
    pal565[idx] = v;
    pal_gen++;
}

bool v8video_init(void)
{
    if (!vram) {
        vram = heap_caps_malloc(V8_VRAM_SIZE, MALLOC_CAP_SPIRAM);
        if (!vram) {
            ESP_LOGE(TAG, "VRAM alloc (%d KB) failed", V8_VRAM_SIZE / 1024);
            return false;
        }
    }
    memset(vram, 0, V8_VRAM_SIZE);
    memset(pvia, 0, sizeof(pvia));
    asc_level = false;
    memset(clut, 0, sizeof(clut));
    memset(vdac_regs, 0, sizeof(vdac_regs));
    vdac_index = 0;
    vdac_comp = 0;
    for (int i = 0; i < 256; i++) pal_update((uint8_t)i);
    /* MAME v8 reset values */
    pvia[0x00] = 0x4f;
    pvia[0x01] = 0x06;
    pvia[0x02] = 0x7f;   /* slot IRQ lines inactive (active low) */
    pvia[0x03] = 0x00;
    ESP_LOGI(TAG, "V8 video: %d KB VRAM at $%06X, sense=%d",
             V8_VRAM_SIZE / 1024, V8_VRAM_BASE, V8_MONITOR_SENSE);
    return true;
}

uint8_t *v8video_get_vram(void) { return vram; }
static int scr_w = 512, scr_h = 384;
int v8video_screen_width(void)  { return scr_w; }
int v8video_screen_height(void) { return scr_h; }

/* ---- Native-LCD mode: 512x384 -> 480x320 declaration-ROM patch ----------
 *
 * The built-in video's sResources live in the main ROM's declaration ROM
 * (pseudo-slot, format block in the last 20 bytes). Each video mode has a
 * VPBlock per depth: [$0000002E size] vpBaseOffset.L vpRowBytes.W
 * vpBounds(t,l,b,r) ... vpPixelSize ... The Slot Manager copies these with
 * byte reads (sReadStruct, PC $A05A36 on LC II) and InitGDevice builds the
 * screen PixMap (bounds, ScreenRow, ScrnBase) from them.
 *
 * We keep a patched copy of the declaration-ROM region in which every
 * VPBlock with bounds (0,0,384,512) becomes (0,0,320,480); rowBytes (1024 for
 * the 512KB-VRAM blocks the driver uses) is left alone. The Slot Manager's
 * CRC (ROL.L #1 / ADD.B over the region, CRC field skipped, checked at
 * $A06084 -> smCRCFail) is recomputed into the copy. mac_memory.c serves
 * the copy for BYTE reads only; the startup ROM checksum ($A46AF0) sums
 * WORDS and therefore still sees the pristine image and passes. */
static uint8_t *decl_copy;

uint32_t v8video_patch_rom(const uint8_t *rom, uint32_t size, const uint8_t **copy)
{
    *copy = NULL;
#if V8_NATIVE_LCD
    if (size < 0x10000) return 0xFFFFFFFF;
    const uint8_t *fb = rom + size - 20;               /* format block */
    if (fb[19] != 0x0F || fb[14] != 0x5A || fb[15] != 0x93) return 0xFFFFFFFF;
    int32_t dir = (int32_t)((fb[0] << 24) | (fb[1] << 16) | (fb[2] << 8) | fb[3]);
    dir = (dir << 8) >> 8;                             /* 24-bit signed */
    uint32_t len = (fb[4] << 24) | (fb[5] << 16) | (fb[6] << 8) | fb[7];
    uint32_t dir_off = (uint32_t)((int32_t)(size - 20) + dir);
    uint32_t start = (len && len < size) ? size - len : dir_off;
    if (dir_off < start) start = dir_off;
    start &= ~3u;
    if (start >= size) return 0xFFFFFFFF;
    uint32_t n = size - start;
    decl_copy = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!decl_copy) return 0xFFFFFFFF;
    memcpy(decl_copy, rom + start, n);

    int patched = 0;
    for (uint32_t i = 0; i + 4 + 14 <= n; i += 2) {
        uint8_t *b = decl_copy + i;
        if (b[0] || b[1] || b[2] || b[3] != 0x2E) continue;
        uint8_t *vp = b + 4;                           /* VPBlock */
        if (vp[6] | vp[7] | vp[8] | vp[9]) continue;  /* top, left = 0 */
        if (vp[10] != 0x01 || vp[11] != 0x80 || vp[12] != 0x02 || vp[13] != 0x00) continue;
        vp[10] = 0x01; vp[11] = 0x40;                 /* bottom 384 -> 320 */
        vp[12] = 0x01; vp[13] = 0xE0;                 /* right  512 -> 480 */
        patched++;
    }
    if (!patched) {
        heap_caps_free(decl_copy);
        decl_copy = NULL;
        return 0xFFFFFFFF;
    }
    /* Recompute the Slot Manager CRC when the ROM declares one */
    if (len && len <= n) {
        const uint8_t *p = decl_copy + (n - len);
        uint32_t crc = 0;
        for (uint32_t i = 0; i < len; i++) {
            uint32_t rem = len - i;
            crc = (crc << 1) | (crc >> 31);
            if (!(rem > 8 && rem <= 12)) crc += p[i];
        }
        uint8_t *cf = decl_copy + n - 12;
        cf[0] = crc >> 24; cf[1] = crc >> 16; cf[2] = crc >> 8; cf[3] = crc;
    }
    scr_w = 480;
    scr_h = 320;
    ESP_LOGI(TAG, "native LCD: %d VPBlocks 512x384 -> 480x320 (decl ROM $%05X-$%05X, %u B copy)",
             patched, (unsigned)start, (unsigned)size, (unsigned)n);
    *copy = decl_copy;
    return start;
#else
    (void)rom; (void)size;
    return 0xFFFFFFFF;
#endif
}
uint8_t  v8video_get_monp(void) { return pvia[0x10]; }
const uint8_t *v8video_get_clut(void) { return clut; }
const uint16_t *v8video_get_pal565(void) { return pal565; }
uint32_t v8video_get_pal_gen(void) { return pal_gen; }

/* Pseudo-VIA2 interrupt: IFR bit 1 ("any slot") follows the active-low
 * slot lines in reg $02; IFR & IER ($13) drives CPU IRQ level 2 (MAME v8
 * pseudovia_recalc_irqs). Slot E's VBL runs the slot VBL queue, which is
 * where the cursor task (jCrsrTask) gets called on Mac II-class ROMs. */
static void pvia_recalc(void)
{
    if (asc_level) pvia[0x03] |= 0x10;      /* a level: software cannot clear it */
    else           pvia[0x03] &= (uint8_t)~0x10;
    if (~pvia[0x02] & 0x78) pvia[0x03] |= 0x02;
    else                    pvia[0x03] &= (uint8_t)~0x02;
    via_update_cpu_irq();
}

bool v8video_irq_pending(void)
{
    return (pvia[0x03] & pvia[0x13] & 0x7F) != 0;
}

void v8video_asc_irq(bool active)
{
    asc_level = active;
    pvia_recalc();
}

void v8video_vbl(bool active)
{
    if (active) pvia[0x02] &= (uint8_t)~0x40;   /* slot E (built-in video) VBL, active low */
    else        pvia[0x02] |= 0x40;
    pvia_recalc();
}

static uint8_t pvia_read(uint32_t off)
{
    uint8_t v;
    if (off >= 0x100) {
        /* VIA-style aliases: reg 13 = IFR, reg 14 = IER */
        switch ((off >> 9) & 0xF) {
        case 13: return pvia[0x03];
        case 14: return pvia[0x13];
        default: return 0;
        }
    }
    off &= 0x1F;
    v = pvia[off];
    if (off == 0x10) {
        v = (uint8_t)((v & ~0x38) | (V8_MONITOR_SENSE << 3));
    } else if (off == 0x03) {
        v &= 0x7F;
        if (v & pvia[0x13] & 0x7F) v |= 0x80;
    } else if (off == 0x12 || off == 0x13) {
        v &= 0x7F;
    }
    return v;
}

static void pvia_write(uint32_t off, uint8_t value)
{
    if (off >= 0x100) {
        switch ((off >> 9) & 0xF) {
        case 13: off = 0x03; break;
        case 14: off = 0x13; break;
        default: return;
        }
    }
    off &= 0x1F;
    switch (off) {
    case 0x02:              /* slot IFR: writing 1 to bit 6 clears the VBL flag */
        pvia[0x02] |= value & 0x40;
        pvia_recalc();
        break;
    case 0x03:              /* IFR: write 1s to clear */
        pvia[0x03] &= (uint8_t)~(value & 0x7F);
        pvia_recalc();
        break;
    case 0x12:
    case 0x13:              /* IERs: bit 7 = set/clear */
        if (value & 0x80) pvia[off] |= value & 0x7F;
        else              pvia[off] &= (uint8_t)~(value & 0x7F);
        {
            static int ier_log;
            if (ier_log < 8) {
                ier_log++;
                ESP_LOGI(TAG, "pVIA2 %s <- $%02X (now $%02X)",
                         off == 0x12 ? "SIER" : "IER", value, pvia[off]);
            }
        }
        pvia_recalc();
        break;
    case 0x10: {
        uint8_t old = pvia[0x10];
        pvia[0x10] = value;
        if ((old ^ value) & 0x47) {
            ESP_LOGI(TAG, "MonP $%02X->$%02X (depth=%d video %s)", old, value,
                     1 << (value & 7), (value & 0x40) ? "off" : "on");
        }
        break;
    }
    default:
        pvia[off] = value;
        break;
    }
}

uint8_t v8video_read(uint32_t address)
{
    if (address >= V8_VRAM_BASE && address < V8_VRAM_BASE + V8_VRAM_SIZE) {
        return vram[address - V8_VRAM_BASE];
    }
    if (address >= 0xF26000 && address < 0xF28000) {
        return pvia_read(address - 0xF26000);
    }
    if (address >= 0xF24000 && address < 0xF26000) {
        uint32_t r = address & 0xF;
        if (r == 1) {
            uint8_t v = clut[vdac_index * 3 + vdac_comp];
            if (++vdac_comp == 3) { vdac_comp = 0; vdac_index++; }
            return v;
        }
        if (r == 0) return vdac_index;
        return vdac_regs[r];
    }
    return 0xFF;
}

void v8video_write(uint32_t address, uint8_t value)
{
    if (address >= V8_VRAM_BASE && address < V8_VRAM_BASE + V8_VRAM_SIZE) {
        vram[address - V8_VRAM_BASE] = value;
        return;
    }
    if (address >= 0xF26000 && address < 0xF28000) {
        pvia_write(address - 0xF26000, value);
        return;
    }
    if (address >= 0xF24000 && address < 0xF26000) {
        uint32_t r = address & 0xF;
        if (r == 0) {
            vdac_index = value;
            vdac_comp = 0;
        } else if (r == 1) {
            clut[vdac_index * 3 + vdac_comp] = value;
            if (++vdac_comp == 3) { pal_update(vdac_index); vdac_comp = 0; vdac_index++; }
        } else {
            if (r == 2 && vdac_regs[2] != value)
                ESP_LOGI(TAG, "VDAC ctl $%02X->$%02X", vdac_regs[2], value);
            vdac_regs[r] = value;
        }
    }
}

#endif /* MAC_ROM_LC_II */

#if MAC_ROM_LC_II && defined(HOST_VIDPROBE)
#include <stdio.h>
void v8video_host_dump(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(&pvia[0x10], 1, 1, f);
    fwrite(clut, 1, sizeof(clut), f);
    fwrite(vram, 1, V8_VRAM_SIZE, f);
    fclose(f);
}
static void __attribute__((destructor)) v8video_host_dump_at_exit(void)
{
    if (vram) v8video_host_dump("v8vram.bin");
}
#endif
