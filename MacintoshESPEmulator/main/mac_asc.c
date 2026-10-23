#include "mac_asc.h"

#if MAC_SOUND

#include <string.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "m68k.h"
#include "mac_emulator.h"
#include "mac_v8video.h"

#if MAC_CYCLE_SCALE != 1
/* At scale 2 the 68k cannot keep the FIFO ahead of a 22 kHz drain (the ROM's
 * chime generator then never sees "full" and boot never finishes). */
#error MAC_SOUND needs MAC_V8_CYCLE_SCALE=1
#endif

static const char *TAG = "asc";

#define FIFO_SIZE 1024
#define FIFO_HALF 512
#define RING_FRAMES 8192            /* ~185 ms at 44.1 kHz */

/* $800 version: $E8 = original ASC (the LC II); the ROM tests for $B0 = EASC */
#ifndef ASC_VERSION
#define ASC_VERSION 0xE8
#endif

#define R_MODE      0x801           /* 0 off, 1 FIFO, 2 wavetable */
#define R_FIFOCTL   0x803           /* bit 7: clear both FIFOs */
#define R_STATUS    0x804           /* bit0 A half-empty, bit1 A full, bit2/3 same for B */
#define R_VOLUME    0x806
#define R_CLOCK     0x807

typedef struct {
    uint8_t  buf[FIFO_SIZE];
    uint16_t head;
    uint16_t count;
    bool     written;               /* anything pushed since the last clear */
} fifo_t;

static fifo_t fifo[2];
static uint8_t regs[0x800];         /* registers $800-$FFF, as written */
static bool irq_force;              /* FIFO mode just started: interrupt until acknowledged */
static bool irq_level;              /* what the chip drives on its interrupt line now */

/* output chain: ASC sample clock -> linear interpolation -> 44.1 kHz ring */
static uint64_t out_acc;            /* device cycles * ASC_OUT_RATE not yet turned into samples */
static uint32_t phase;              /* Q16 position between prev_s and cur_s */
static int16_t  prev_s, cur_s;
static int16_t  ring[RING_FRAMES * 2] EXT_RAM_BSS_ATTR;
static volatile uint32_t ring_head, ring_tail;
static uint32_t overruns;

static uint32_t asc_rate_hz(void)
{
    static const uint32_t rates[4] = { 22254, 22050, 44100, 44800 };
    return rates[regs[R_CLOCK - 0x800] & 3];
}

static bool asc_enabled = true;

/* The interrupt line is a level: high while the software feeds a FIFO that is
 * below half full (or just after FIFO mode starts, until the status is read).
 * Software cannot clear it by writing the pseudo-VIA2 IFR (v8video_asc_irq
 * re-applies the level), only by refilling the FIFO or reading $804. */
static void irq_update(void)
{
    bool on = irq_force ||
              (fifo[0].written && fifo[0].count < FIFO_HALF) ||
              (fifo[1].written && fifo[1].count < FIFO_HALF);
    if (on != irq_level) {
        irq_level = on;
        v8video_asc_irq(on);
    }
}

static void fifo_clear(void)
{
    memset(fifo, 0, sizeof(fifo));
    prev_s = cur_s = 0;
    irq_force = false;
    irq_update();
}

void asc_init(void)
{
    memset(regs, 0, sizeof(regs));
    out_acc = 0;
    phase = 0;
    ring_head = ring_tail = 0;
    irq_force = irq_level = false;
    fifo_clear();
}

static uint8_t status(void)
{
    uint8_t s = 0;
    for (int i = 0; i < 2; i++) {
        if (fifo[i].count < FIFO_HALF)  s |= 1 << (i * 2);
        if (fifo[i].count >= FIFO_SIZE) s |= 2 << (i * 2);
    }
    return s;
}

void asc_set_enabled(bool on)
{
    asc_enabled = on;
}

uint8_t asc_read(uint32_t address)
{
    uint32_t off = address & 0xFFF;
    if (!asc_enabled) return 0xFF;      /* as if the chip were absent */
    if (off < 0x800) return 0;
    if (off == 0x800) return ASC_VERSION;
    if (off == R_STATUS) {
        uint8_t s = status();
        irq_force = false;          /* reading the status acknowledges the start-up interrupt */
        irq_update();
        return s;
    }
    return regs[off - 0x800];
}

static void fifo_push(int which, uint8_t v)
{
    fifo_t *f = &fifo[which];
    f->written = true;
    if (f->count < FIFO_SIZE) {
        f->buf[(f->head + f->count) % FIFO_SIZE] = v;
        f->count++;
    }
}

void asc_write(uint32_t address, uint8_t value)
{
    uint32_t off = address & 0xFFF;
    if (!asc_enabled) return;
    if (off < 0x800) {
        if (regs[R_MODE - 0x800] == 1) {
            fifo_push(off >= 0x400, value);
            irq_update();
        }
        return;
    }
    switch (off) {
    case 0x800:
        return;                     /* version is read-only */
    case R_MODE:
        if (value == 2) ESP_LOGW(TAG, "wavetable mode not emulated");
        if (value != 1) {
            if (regs[R_MODE - 0x800] == 1) fifo_clear();
        }
        break;
    case R_FIFOCTL:
        if (value & 0x80) fifo_clear();
        break;
    case R_STATUS:
        return;
    default:
        break;
    }
    regs[off - 0x800] = value;
    if (off == R_MODE && value == 1) {
        irq_force = true;           /* empty FIFOs are "half empty" */
        irq_update();
    }
}

static uint8_t fifo_pop(int which, bool *had)
{
    fifo_t *f = &fifo[which];
    if (!f->count) {
        *had = false;
        return 0x80;
    }
    uint8_t v = f->buf[f->head];
    f->head = (f->head + 1) % FIFO_SIZE;
    f->count--;
    *had = true;
    return v;
}

/* One ASC sample period: take one byte from each FIFO. LC II output is
 * mono: A alone if nothing ever went to B, else their average. */
static int16_t next_sample(void)
{
    bool ha, hb;
    uint8_t a = fifo_pop(0, &ha);
    uint8_t b = fifo_pop(1, &hb);
    irq_update();
    int v = (fifo[1].written ? (a + b) / 2 : a) - 0x80;
    return (int16_t)(v * 256 * (regs[R_VOLUME - 0x800] >> 5) / 7);   /* $806 bits 7-5: volume */
}

void asc_tick(uint32_t device_cycles)
{
    if (!asc_enabled || regs[R_MODE - 0x800] != 1) return;
    if (!fifo[0].count && !fifo[1].count && !cur_s && !prev_s) {
        /* Nothing to play (the Sound Manager leaves FIFO mode on for good):
         * no samples, so no ring backlog and no per-sample work. */
        out_acc = 0;
        phase = 0;
        irq_update();
        return;
    }
    out_acc += (uint64_t)device_cycles * ASC_OUT_RATE;
    uint32_t n = (uint32_t)(out_acc / MAC_CPU_HZ);
    out_acc -= (uint64_t)n * MAC_CPU_HZ;
    uint32_t step = (uint32_t)(((uint64_t)asc_rate_hz() << 16) / ASC_OUT_RATE);
    while (n--) {
        phase += step;
        while (phase >= 0x10000) {
            phase -= 0x10000;
            prev_s = cur_s;
            cur_s = next_sample();
        }
        int16_t s = (int16_t)(prev_s + (((int32_t)(cur_s - prev_s) * (int32_t)phase) >> 16));
        uint32_t next = (ring_head + 1) % RING_FRAMES;
        if (next == ring_tail) {
            overruns++;
            continue;
        }
        ring[ring_head * 2] = s;
        ring[ring_head * 2 + 1] = s;
        ring_head = next;
    }
}

uint32_t asc_pull(int16_t *stereo, uint32_t frames)
{
    uint32_t got = 0;
    while (got < frames && ring_tail != ring_head) {
        stereo[got * 2] = ring[ring_tail * 2];
        stereo[got * 2 + 1] = ring[ring_tail * 2 + 1];
        ring_tail = (ring_tail + 1) % RING_FRAMES;
        got++;
    }
    return got;
}

bool asc_backlog(void)
{
    uint32_t used = (ring_head + RING_FRAMES - ring_tail) % RING_FRAMES;
    return used > RING_FRAMES / 4;
}

#endif /* MAC_SOUND */
