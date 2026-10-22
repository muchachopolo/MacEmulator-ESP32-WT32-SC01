/*
 * Writable boot disk: block copy-on-write overlay in PSRAM.
 * See mac_disk_overlay.h.
 */
#include "mac_disk_overlay.h"

#include <string.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

static const char *TAG = "diskov";

uint32_t disk_ov_pool_kb = DISK_OVERLAY_KB;

static const uint8_t *img;        /* flash image */
static uint32_t img_size;
static uint32_t n_blocks;         /* image blocks (rounded up) */
static uint16_t *slot_of;         /* per block: 0 = flash, n = pool slot n-1 */
static uint8_t  *pool;            /* n_slots * 512 */
static uint32_t n_slots;
static uint32_t used;             /* slots handed out */
static bool     active;

/* stats */
static uint32_t st_writes, st_wblocks, st_same, st_full;
static int64_t  st_last_log;
static uint32_t st_last_used = ~0u;

bool disk_ov_active(void) { return active; }

uint32_t disk_ov_write_count(void) { return st_writes; }

bool disk_ov_init(const uint8_t *image, uint32_t size)
{
    img = image;
    img_size = size;
    active = false;
#if DISK_WRITABLE
    if (!image || !size) return false;
    n_blocks = (size + DISK_OV_BLOCK - 1) / DISK_OV_BLOCK;
    n_slots = disk_ov_pool_kb * 1024u / DISK_OV_BLOCK;
    if (n_slots > n_blocks) n_slots = n_blocks;     /* never need more */
    if (n_slots > 0xFFFE) n_slots = 0xFFFE;
    slot_of = heap_caps_calloc(n_blocks, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    pool = heap_caps_malloc((size_t)n_slots * DISK_OV_BLOCK, MALLOC_CAP_SPIRAM);
    if (!slot_of || !pool || !n_slots) {
        ESP_LOGE(TAG, "overlay alloc failed (%lu KB pool + %lu B index): disk read-only",
                 (unsigned long)(n_slots / 2), (unsigned long)(n_blocks * 2));
        heap_caps_free(slot_of); heap_caps_free(pool);
        slot_of = NULL; pool = NULL;
        return false;
    }
    used = 0;
    active = true;
    ESP_LOGW(TAG, "writable disk: %lu-block image, overlay pool %lu blocks (%lu KB) + %lu B index in PSRAM",
             (unsigned long)n_blocks, (unsigned long)n_slots,
             (unsigned long)(n_slots / 2), (unsigned long)(n_blocks * 2));
    return true;
#else
    return false;
#endif
}

static inline const uint8_t *block_src(uint32_t blk)
{
    uint16_t s = slot_of[blk];
    return s ? pool + (uint32_t)(s - 1) * DISK_OV_BLOCK : img + blk * DISK_OV_BLOCK;
}

void disk_ov_read(uint8_t *dst, uint32_t off, uint32_t cnt)
{
    if (!active || used == 0) {             /* nothing written yet */
        memcpy(dst, img + off, cnt);
        return;
    }
    while (cnt) {
        uint32_t blk = off / DISK_OV_BLOCK, in = off % DISK_OV_BLOCK;
        uint32_t n = DISK_OV_BLOCK - in;
        if (n > cnt) n = cnt;
        if (off + n > img_size) n = img_size - off;   /* partial last block */
        memcpy(dst, block_src(blk) + in, n);
        dst += n; off += n; cnt -= n;
    }
}

uint8_t disk_ov_byte(uint32_t off)
{
    if (!active || used == 0) return img[off];
    return block_src(off / DISK_OV_BLOCK)[off % DISK_OV_BLOCK];
}

void disk_ov_log_stats(const char *why)
{
    if (!active) return;
    ESP_LOGW(TAG, "%s: %lu/%lu blocks used (%lu/%lu KB), %lu writes, %lu blocks written, %lu unchanged, %lu disk-full",
             why, (unsigned long)used, (unsigned long)n_slots,
             (unsigned long)(used / 2), (unsigned long)(n_slots / 2),
             (unsigned long)st_writes, (unsigned long)st_wblocks,
             (unsigned long)st_same, (unsigned long)st_full);
}

int disk_ov_write(const uint8_t *src, uint32_t off, uint32_t cnt)
{
    if (!active) return DISK_OV_DSKFUL;
    st_writes++;

    /* Pass 1: count the new slots this request needs (all or nothing). */
    uint32_t need = 0;
    for (uint32_t o = off, c = cnt; c; ) {
        uint32_t blk = o / DISK_OV_BLOCK, in = o % DISK_OV_BLOCK;
        uint32_t n = DISK_OV_BLOCK - in;
        if (n > c) n = c;
        if (o + n > img_size) n = img_size - o;
        if (!slot_of[blk] && memcmp(img + o, src + (o - off), n) != 0) need++;
        o += n; c -= n;
    }
    if (need > n_slots - used) {
        st_full++;
        if (st_full <= 10 || (st_full % 100) == 0)
            disk_ov_log_stats("POOL FULL, write refused (dskFulErr)");
        return DISK_OV_DSKFUL;
    }

    /* Pass 2: copy. */
    while (cnt) {
        uint32_t blk = off / DISK_OV_BLOCK, in = off % DISK_OV_BLOCK;
        uint32_t n = DISK_OV_BLOCK - in;
        if (n > cnt) n = cnt;
        if (off + n > img_size) n = img_size - off;
        uint16_t s = slot_of[blk];
        if (!s) {
            if (memcmp(img + off, src, n) == 0) {
                st_same++;                  /* identical to flash: no slot */
                src += n; off += n; cnt -= n;
                continue;
            }
            s = (uint16_t)(++used);
            uint8_t *d = pool + (uint32_t)(s - 1) * DISK_OV_BLOCK;
            uint32_t bo = blk * DISK_OV_BLOCK;
            uint32_t bl = img_size - bo < DISK_OV_BLOCK ? img_size - bo : DISK_OV_BLOCK;
            memcpy(d, img + bo, bl);        /* rest of a partial block */
            slot_of[blk] = s;
        }
        memcpy(pool + (uint32_t)(s - 1) * DISK_OV_BLOCK + in, src, n);
        st_wblocks++;
        src += n; off += n; cnt -= n;
    }

    /* Usage log: first write, then at most every 2 s when it changed. */
    int64_t now = esp_timer_get_time();
    if (st_writes == 1 || (used != st_last_used && now - st_last_log > 2000000)) {
        disk_ov_log_stats("usage");
        st_last_log = now;
        st_last_used = used;
    }
    return 0;
}

int disk_ov_xfer(bool write, uint8_t *rambuf, uint32_t off, uint32_t cnt)
{
    if (write && active) return disk_ov_write(rambuf, off, cnt);
    disk_ov_read(rambuf, off, cnt);
    return 0;
}
