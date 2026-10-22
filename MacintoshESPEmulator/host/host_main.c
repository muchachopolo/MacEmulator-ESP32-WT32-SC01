/* Host (Linux/WSL) driver for the ESP32 Macintosh emulator core.
 *
 * Runs mac_emu_init() + mac_emu_run() unmodified. The per-frame hook
 * (taskYIELD() in the shim, called once at the end of every emulated
 * frame) enforces --frames, renders screenshots, and the atexit handler
 * dumps RAM and prints speed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

#include "mac_emulator.h"
#include "mac_memory.h"
#include "mac_iwm.h"
#include "mac_disk_overlay.h"
#include "mac_video.h"
#include "mac_kbd.h"
#include "mac_v8disk.h"
#include "display.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "m68k.h"

FILE *host_log_file = NULL;
extern uint16_t host_lcd[DISPLAY_WIDTH * DISPLAY_HEIGHT];

static long  opt_frames = 0;          /* 0 = run forever */
static long  opt_shot_every = 300;    /* 0 = no screenshots */
static const char *opt_dump_ram = NULL;
static const char *opt_disk_rw = NULL;   /* --disk-rw FILE */
static const char *opt_disk_out = NULL;  /* --disk-out FILE */
static const char *opt_disk2_out = NULL; /* --disk2-out FILE (LC II, --disk2) */
static const char *opt_out = "out";
static int   opt_cpu = 0;             /* 0 = keep what mac_emu_init() chose */
static long  frames = 0;
static int64_t t_start_us;
static long  opt_vclock = 0;          /* --vclock US: virtual clock, US microseconds per frame */
int64_t host_vclock_us = -1;          /* read by shim/esp_timer.h; -1 = real clock */

/* Wall clock that ignores --vclock (speed report, log timestamps). */
static int64_t real_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

uint32_t esp_log_timestamp(void)
{
    return (uint32_t)((real_time_us() - t_start_us) / 1000);
}

void vTaskDelay(TickType_t ticks) { (void)ticks; }

/* ---------------- minimal PNG writer (no deps) ----------------
 * zlib stream made of uncompressed "stored" deflate blocks; files are
 * ~raw size (300KB-ish), which is fine for debugging screenshots. */
static uint32_t crc_table[256];
static uint32_t crc32_upd(uint32_t crc, const uint8_t *d, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *d++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void png_chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
    uint8_t hdr[8];
    put32(hdr, n);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint8_t c[4];
    put32(c, crc32_upd(crc32_upd(0, hdr + 4, 4), d, n));
    fwrite(c, 1, 4, f);
}

/* raw: h rows of `stride` bytes (filter byte NOT included). */
static void write_png(const char *path, int w, int h, int bitdepth, int colortype,
                      const uint8_t *raw, int stride)
{
    size_t fsz = (size_t)(stride + 1) * h;
    uint8_t *filt = malloc(fsz);
    for (int y = 0; y < h; y++) {
        filt[y * (stride + 1)] = 0;   /* filter: none */
        memcpy(filt + y * (stride + 1) + 1, raw + (size_t)y * stride, stride);
    }
    size_t nblk = (fsz + 65534) / 65535;
    uint8_t *z = malloc(2 + fsz + nblk * 5 + 4), *o = z;
    *o++ = 0x78; *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t pos = 0; pos < fsz; ) {
        size_t n = fsz - pos > 65535 ? 65535 : fsz - pos;
        *o++ = (pos + n == fsz);              /* BFINAL, BTYPE=00 */
        *o++ = n; *o++ = n >> 8; *o++ = ~n; *o++ = ~n >> 8;
        memcpy(o, filt + pos, n);
        for (size_t i = 0; i < n; i++) { a = (a + filt[pos + i]) % 65521; b = (b + a) % 65521; }
        o += n; pos += n;
    }
    put32(o, (b << 16) | a); o += 4;
    FILE *f = fopen(path, "wb");
    if (f) {
        static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
        fwrite(sig, 1, 8, f);
        uint8_t ihdr[13] = { 0 };
        put32(ihdr, w); put32(ihdr + 4, h);
        ihdr[8] = bitdepth; ihdr[9] = colortype;
        png_chunk(f, "IHDR", ihdr, 13);
        png_chunk(f, "IDAT", z, (uint32_t)(o - z));
        png_chunk(f, "IEND", NULL, 0);
        fclose(f);
    } else {
        perror(path);
    }
    free(z);
    free(filt);
}

static void take_screenshots(void)
{
    uint8_t *ram = mac_mem_get_ram();
    uint32_t ram_size = mac_mem_get_ram_size();
    if (!ram || !ram_size) return;
    char path[512];

    /* 1) What the LCD would show: mac_video_render() -> display_draw(). */
    mac_video_render(ram, ram_size);
    static uint8_t rgb[DISPLAY_WIDTH * DISPLAY_HEIGHT * 3];
    for (int i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++) {
        uint16_t p = host_lcd[i];
        rgb[i * 3 + 0] = ((p >> 11) & 0x1F) * 255 / 31;
        rgb[i * 3 + 1] = ((p >> 5) & 0x3F) * 255 / 63;
        rgb[i * 3 + 2] = (p & 0x1F) * 255 / 31;
    }
    snprintf(path, sizeof path, "%s/frame_%05ld.png", opt_out, frames);
    write_png(path, DISPLAY_WIDTH, DISPLAY_HEIGHT, 8, 2, rgb, DISPLAY_WIDTH * 3);

    /* 2) Raw 512x342 1-bit Mac framebuffer at ScrnBase ($0824), same
     *    address resolution as mac_video_render. Mac 1=black -> invert. */
    uint32_t sb = ((uint32_t)ram[0x824] << 24) | (ram[0x825] << 16) |
                  (ram[0x826] << 8) | ram[0x827];
    if (sb == 0) sb = ram_size - 0x5900;
    else if (sb >= ram_size) sb &= ram_size - 1;
    static uint8_t bits[MAC_SCREEN_SIZE];
    for (uint32_t i = 0; i < MAC_SCREEN_SIZE; i++)
        bits[i] = ~ram[(sb + i) & (ram_size - 1)];
    snprintf(path, sizeof path, "%s/scrn_%05ld.png", opt_out, frames);
    write_png(path, MAC_SCREEN_WIDTH, MAC_SCREEN_HEIGHT, 1, 0, bits, MAC_SCREEN_STRIDE);
    fprintf(stderr, "H (%u) host: F%ld screenshot (ScrnBase=$%06X) -> %s/{frame,scrn}_%05ld.png\n",
            (unsigned)esp_log_timestamp(), frames, (unsigned)sb, opt_out, frames);
}

/* ---------------- per-frame hook / exit ---------------- */
long host_frame_count(void) { return frames; }
extern long host_taps[16];
extern int  host_ntaps;
struct host_swipe { long f, n; int dx, dy; };
extern struct host_swipe host_swipes[16];
extern int host_nswipes;

/* Scripted keyboard (MAC_KEYBOARD builds): --type F:TEXT, --key F:HH[+HH..].
 * Each key goes down at its frame and up HOST_KEY_HOLD frames later; typed
 * characters are HOST_KEY_GAP frames apart. */
#define HOST_KEY_HOLD 6
#define HOST_KEY_GAP  12
static struct { long f; uint8_t code; bool down; } host_keys[1024];
static int host_nkeys;

static void host_key_add(long f, uint8_t code, bool down)
{
    if (host_nkeys < (int)(sizeof host_keys / sizeof host_keys[0])) {
        host_keys[host_nkeys].f = f;
        host_keys[host_nkeys].code = code;
        host_keys[host_nkeys].down = down;
        host_nkeys++;
    }
}

/* US layout: ASCII -> ADB key code, bit 7 = needs Shift */
static int host_ascii_adb(char c)
{
    static const struct { char c; uint8_t k; } map[] = {
        {'a',0x00},{'s',0x01},{'d',0x02},{'f',0x03},{'h',0x04},{'g',0x05},{'z',0x06},{'x',0x07},
        {'c',0x08},{'v',0x09},{'b',0x0B},{'q',0x0C},{'w',0x0D},{'e',0x0E},{'r',0x0F},{'y',0x10},
        {'t',0x11},{'1',0x12},{'2',0x13},{'3',0x14},{'4',0x15},{'6',0x16},{'5',0x17},{'=',0x18},
        {'9',0x19},{'7',0x1A},{'-',0x1B},{'8',0x1C},{'0',0x1D},{']',0x1E},{'o',0x1F},{'u',0x20},
        {'[',0x21},{'i',0x22},{'p',0x23},{'\n',0x24},{'l',0x25},{'j',0x26},{'\'',0x27},{'k',0x28},
        {';',0x29},{'\\',0x2A},{',',0x2B},{'/',0x2C},{'n',0x2D},{'m',0x2E},{'.',0x2F},{'\t',0x30},
        {' ',0x31},{'`',0x32},
        {'!',0x92},{'@',0x93},{'#',0x94},{'$',0x95},{'^',0x96},{'%',0x97},{'+',0x98},{'(',0x99},
        {'&',0x9A},{'_',0x9B},{'*',0x9C},{')',0x9D},{'}',0x9E},{'{',0xA1},{'"',0xA7},{':',0xA9},
        {'|',0xAA},{'<',0xAB},{'?',0xAC},{'>',0xAF},{'~',0xB2},
    };
    if (c >= 'A' && c <= 'Z') return host_ascii_adb((char)(c - 'A' + 'a')) | 0x80;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (map[i].c == c) return map[i].k;
    return -1;
}

#if MAC_ROM_LC_II
/* --disk2 FILE (LC II, MAC_DRIVE2=1): second disk, in memory, writable */
static uint8_t *host_d2;
static uint32_t host_d2_size;
static int host_d2_io(bool write, uint8_t *buf, uint32_t off, uint32_t cnt)
{
    if (write) memcpy(host_d2 + off, buf, cnt);
    else       memcpy(buf, host_d2 + off, cnt);
    return 0;
}
#endif

static void host_keys_frame(void)
{
    for (int i = 0; i < host_nkeys; i++)
        if (host_keys[i].f == frames) kbd_key(host_keys[i].code, host_keys[i].down);
}

void host_frame_hook(void)
{
    frames++;
    host_keys_frame();
    if (opt_vclock > 0) host_vclock_us = (int64_t)frames * opt_vclock;
    /* Event queue sanity: EvQHdr.qHead/qTail ($014C/$0150) must never
     * point into the vector table ($00-$FF), as the old Classic-style
     * mouse posting at $C0/$D8 did on V8. */
    {
        uint8_t *r = mac_mem_get_ram();
        static int evq_bad;
        if (r) {
            uint32_t h = (r[0x14C] << 24) | (r[0x14D] << 16) | (r[0x14E] << 8) | r[0x14F];
            uint32_t t = (r[0x150] << 24) | (r[0x151] << 16) | (r[0x152] << 8) | r[0x153];
            /* Drive 1 DrvSts.diskInPlace (byte before the queue element's
             * qLink - 3): negative = ejected, 0 = no disk. */
            static int dip = -1000;
            uint32_t q = ((r[0x30A] << 24) | (r[0x30B] << 16) | (r[0x30C] << 8) | r[0x30D]) & 0xFFFFFF;
            if (q >= 4 && q < 0x200000 && r[q + 7] == 1 && r[q + 6] == 0) {
                int v = r[q - 3];
                if (dip != -1000 && v != dip)
                    fprintf(stderr, "H host: F%ld drive 1 diskInPlace $%02X -> $%02X\n", frames, dip, v);
                dip = v;
            }
            /* CurApName ($910, Pascal string): boot-to-Finder marker */
            static char apn[33] = "?";
            int al = r[0x910];
            if (al < 32) {
                char cur[33];
                memcpy(cur, r + 0x911, al);
                cur[al] = 0;
                static int apn_n;   /* background-time switches repeat this */
                if (strcmp(cur, apn) != 0) {
                    if (apn_n++ < 200)
                        fprintf(stderr, "H host: F%ld CurApName \"%s\"\n", frames, cur);
                    strcpy(apn, cur);
                }
            }
            /* Time ($20C, seconds since 1904): should advance 1 per 60 frames */
            if (frames % 3000 == 0)
                fprintf(stderr, "H host: F%ld Time $%08X\n", frames,
                        (unsigned)((r[0x20C] << 24) | (r[0x20D] << 16) | (r[0x20E] << 8) | r[0x20F]));
            if (((h && h < 0x100) || (t && t < 0x100)) && evq_bad < 10) {
                evq_bad++;
                fprintf(stderr, "H host: F%ld EVQ-BAD qHead=$%08X qTail=$%08X\n",
                        frames, (unsigned)h, (unsigned)t);
            }
        }
    }
    if (opt_shot_every > 0 && frames % opt_shot_every == 0)
        take_screenshots();
    if (opt_frames > 0 && frames >= opt_frames)
        exit(0);
}

#ifdef HOST_TRACE
/* host_prof.c */
extern long prof_from, prof_to;
extern const char *prof_out;
extern int prof_enabled, prof_top;
void prof_instr(uint32_t pc);
void prof_report(void);

/* Per-instruction trace (make TRACE=1): first opt_trace instructions go to
 * opt_trace_file; a ring of the last TRACE_RING is dumped at exit. */
#define TRACE_RING 4096
static long opt_trace = 0;
static uint32_t opt_stop_lo, opt_stop_hi;   /* --stop-pc LO-HI (24-bit) */
static long opt_stop_from;                  /* --stop-from F: arm --stop-pc at frame F */
static const char *opt_trace_file = "trace.txt";
static FILE *trace_f;
static unsigned long trace_n;
static struct { uint32_t pc, sr, sp, d0, a0, a1, a4, a6; long frame; } trace_ring[TRACE_RING];

static uint32_t opt_watch;                  /* --watch ADDR: report byte changes */
static long watch_n;

void host_trace_hook(unsigned int pc)
{
    if (prof_enabled) prof_instr(pc);
    if (opt_watch && watch_n < 200 && frames >= opt_stop_from) {
        static int last = -1;
        uint8_t *r = mac_mem_get_ram();
        if (r) {
            int v = r[opt_watch];
            if (last >= 0 && v != last) {
                watch_n++;
                fprintf(stderr, "H host: F%ld watch $%06X $%02X -> $%02X before PC=%08X (prev PC=%08X)\n",
                        frames, (unsigned)opt_watch, last, v, pc,
                        trace_n ? trace_ring[(trace_n - 1) % TRACE_RING].pc : 0);
            }
            last = v;
        }
    }
    unsigned i = trace_n % TRACE_RING;
    trace_ring[i].pc = pc;
    trace_ring[i].sr = m68k_get_reg(NULL, M68K_REG_SR);
    trace_ring[i].sp = m68k_get_reg(NULL, M68K_REG_SP);
    trace_ring[i].d0 = m68k_get_reg(NULL, M68K_REG_D0);
    trace_ring[i].a0 = m68k_get_reg(NULL, M68K_REG_A0);
    trace_ring[i].a1 = m68k_get_reg(NULL, M68K_REG_A1);
    trace_ring[i].a4 = m68k_get_reg(NULL, M68K_REG_A4);
    trace_ring[i].a6 = m68k_get_reg(NULL, M68K_REG_A6);
    trace_ring[i].frame = frames;
    if ((long)trace_n < opt_trace) {
        if (!trace_f) trace_f = fopen(opt_trace_file, "w");
        if (trace_f)
            fprintf(trace_f, "%lu F%ld PC=%08X SR=%04X SP=%08X D0=%08X A0=%08X A1=%08X A4=%08X A6=%08X\n",
                    trace_n, frames, pc, trace_ring[i].sr, trace_ring[i].sp,
                    trace_ring[i].d0, trace_ring[i].a0, trace_ring[i].a1,
                    trace_ring[i].a4, trace_ring[i].a6);
    }
    trace_n++;
    if (opt_stop_hi && frames >= opt_stop_from && (pc & 0xFFFFFF) >= opt_stop_lo && (pc & 0xFFFFFF) <= opt_stop_hi) {
        fprintf(stderr, "H host: stop-pc hit PC=%08X at instr %lu F%ld\n", pc, trace_n - 1, frames);
        exit(0);
    }
}

static void trace_dump_ring(void)
{
    FILE *f = fopen("trace_ring.txt", "w");
    if (!f) return;
    unsigned long start = trace_n > TRACE_RING ? trace_n - TRACE_RING : 0;
    for (unsigned long n = start; n < trace_n; n++) {
        unsigned i = n % TRACE_RING;
        fprintf(f, "%lu F%ld PC=%08X SR=%04X SP=%08X D0=%08X A0=%08X A1=%08X A4=%08X A6=%08X\n",
                n, trace_ring[i].frame, trace_ring[i].pc, trace_ring[i].sr,
                trace_ring[i].sp, trace_ring[i].d0, trace_ring[i].a0, trace_ring[i].a1,
                trace_ring[i].a4, trace_ring[i].a6);
    }
    fclose(f);
    if (trace_f) fclose(trace_f);
}
#endif

static void at_exit(void)
{
#ifdef HOST_TRACE
    trace_dump_ring();
    prof_report();
#endif
    double secs = (real_time_us() - t_start_us) / 1e6;
    if (secs <= 0) secs = 1e-9;
    double fps = frames / secs;
    fprintf(stderr, "H host: %ld frames in %.2f s = %.1f frames/s = %.0f%% Mac real-time (60 fps)\n",
            frames, secs, fps, fps / 60.0 * 100.0);
    if (opt_dump_ram) {
        uint8_t *ram = mac_mem_get_ram();
        FILE *f = ram ? fopen(opt_dump_ram, "wb") : NULL;
        if (f) {
            fwrite(ram, 1, mac_mem_get_ram_size(), f);
            fclose(f);
            fprintf(stderr, "H host: wrote %u bytes of Mac RAM to %s\n",
                    (unsigned)mac_mem_get_ram_size(), opt_dump_ram);
        } else {
            perror(opt_dump_ram);
        }
    }
    disk_ov_log_stats("exit");
    const char *dout = opt_disk_rw ? opt_disk_rw : opt_disk_out;
    if (dout && iwm_get_disk_data()) {
        /* flash image + PSRAM write overlay (mac_disk_overlay.c) */
        uint32_t n = iwm_get_disk_size();
        uint8_t *img = malloc(n);
        FILE *f = img ? fopen(dout, "wb") : NULL;
        if (img) disk_ov_read(img, 0, n);
        if (f && fwrite(img, 1, n, f) == n) {
            fprintf(stderr, "H host: wrote %u-byte disk image to %s\n", (unsigned)n, dout);
        } else {
            perror(dout);
        }
        if (f) fclose(f);
        free(img);
    }
#if MAC_ROM_LC_II
    if (opt_disk2_out && host_d2) {
        FILE *f = fopen(opt_disk2_out, "wb");
        if (f && fwrite(host_d2, 1, host_d2_size, f) == host_d2_size)
            fprintf(stderr, "H host: wrote %u-byte disk 2 image to %s\n", (unsigned)host_d2_size, opt_disk2_out);
        else
            perror(opt_disk2_out);
        if (f) fclose(f);
    }
#endif
    if (host_log_file) fflush(host_log_file);
}

static void on_signal(int sig) { (void)sig; exit(130); }

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--frames N] [--shot-every K] [--out DIR] [--dump-ram FILE] [--log FILE]\n"
        "  --frames N      stop after N emulated frames (default: run forever)\n"
        "  --shot-every K  write out/frame_NNNNN.png + scrn_NNNNN.png every K frames (0=off, default 300)\n"
        "  --out DIR       screenshot directory (default: out)\n"
        "  --dump-ram FILE write the Mac RAM image to FILE at exit\n"
        "  --log FILE      send emulator log to FILE instead of stderr\n"
        "  --disk-rw FILE  overlay pool = whole image (no disk-full), save the written\n"
        "                  image to FILE at exit (desktop-database prep, see docs/build-guide.md)\n"
        "  --disk-out FILE save the image as written (flash + overlay) to FILE at exit\n"
        "  --cpu 68000|68020|68030  override the CPU type mac_emu_init() set (re-pulses reset)\n"
        "  --tap F                  scripted touch tap starting at frame F (repeatable, max 16)\n"
        "  --type F:TEXT            (MAC_KEYBOARD=1) type TEXT from frame F (\\n = Return)\n"
        "  --key F:HH[+HH..]        (MAC_KEYBOARD=1) press ADB key codes together, e.g. 37+0C = Cmd-Q\n"
        "  --swipe F:dx:dy[:N]      scripted swipe from frame F over N frames (default 2000),\n"
        "                           moving the cursor (dx,dy) screen px; a --tap ending <400 ms\n"
        "                           before it makes it a tap-then-drag (repeatable, max 16)\n"
        "  --trace N       (TRACE=1) first N instructions to trace.txt; ring of last 4096 -> trace_ring.txt\n"
        "  --stop-pc LO-HI (TRACE=1) exit (dumping trace_ring.txt) when PC first enters hex range\n"
        "  --stop-from F   (TRACE=1) only arm --stop-pc from frame F on\n"
        "  --watch ADDR    (TRACE=1) report the PC whenever RAM byte ADDR (hex) changes\n"
        "  --profile-from F / --profile-to F  (TRACE=1) profile instructions in frames [F, T)\n"
        "                  (either option turns the profiler on; report printed at exit)\n"
        "  --profile-out FILE (TRACE=1) also write the profile report to FILE\n"
        "  --profile-top N (TRACE=1) rows per profile table (default 25)\n"
        "  --vclock US     virtual clock: esp_timer_get_time() = frames * US (us), so touch\n"
        "                  polling and gesture timing do not depend on host speed\n", argv0);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if      (!strcmp(a, "--frames") && v)     { opt_frames = atol(v); i++; }
        else if (!strcmp(a, "--shot-every") && v) { opt_shot_every = atol(v); i++; }
        else if (!strcmp(a, "--out") && v)        { opt_out = v; i++; }
        else if (!strcmp(a, "--dump-ram") && v)   { opt_dump_ram = v; i++; }
        else if (!strcmp(a, "--disk-rw") && v)    { opt_disk_rw = v; i++; }
        else if (!strcmp(a, "--disk-out") && v)   { opt_disk_out = v; i++; }
        else if (!strcmp(a, "--disk2-out") && v)  { opt_disk2_out = v; i++; }
        else if (!strcmp(a, "--cpu") && v)       { opt_cpu = atoi(v); i++; }
        else if (!strcmp(a, "--vclock") && v)    { opt_vclock = atol(v); host_vclock_us = 0; i++; }
        else if (!strcmp(a, "--swipe") && v) {
            struct host_swipe w = { 0, 2000, 0, 0 };
            if (sscanf(v, "%ld:%d:%d:%ld", &w.f, &w.dx, &w.dy, &w.n) < 3 || w.n < 1) {
                usage(argv[0]); return 2;
            }
            if (host_nswipes < 16) host_swipes[host_nswipes++] = w;
            i++;
        }
        else if (!strcmp(a, "--type") && v) {
            char *e; long f = strtol(v, &e, 10);
            if (*e != ':') { usage(argv[0]); return 2; }
            for (const char *p = e + 1; *p; p++) {
                char c = *p;
                if (c == '\\' && p[1] == 'n') { c = '\n'; p++; }
                int k = host_ascii_adb(c);
                if (k < 0) continue;
                if (k & 0x80) host_key_add(f, ADB_KEY_SHIFT, true);
                host_key_add(f + 1, (uint8_t)(k & 0x7F), true);
                host_key_add(f + 1 + HOST_KEY_HOLD, (uint8_t)(k & 0x7F), false);
                if (k & 0x80) host_key_add(f + 2 + HOST_KEY_HOLD, ADB_KEY_SHIFT, false);
                f += HOST_KEY_GAP;
            }
            i++;
        }
        else if (!strcmp(a, "--key") && v) {
            char *e; long f = strtol(v, &e, 10);
            uint8_t ks[8]; int nk = 0;
            while (*e == ':' || *e == '+') {
                if (nk < 8) ks[nk++] = (uint8_t)strtoul(e + 1, &e, 16);
                else break;
            }
            if (!nk) { usage(argv[0]); return 2; }
            for (int k = 0; k < nk; k++) host_key_add(f + k, ks[k], true);
            for (int k = nk - 1; k >= 0; k--) host_key_add(f + nk + HOST_KEY_HOLD + (nk - 1 - k), ks[k], false);
            i++;
        }
#if MAC_ROM_LC_II
        else if (!strcmp(a, "--disk2") && v) {
            FILE *f = fopen(v, "rb");
            if (!f) { perror(v); return 1; }
            fseek(f, 0, SEEK_END); host_d2_size = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET);
            host_d2 = malloc(host_d2_size);
            if (!host_d2 || fread(host_d2, 1, host_d2_size, f) != host_d2_size) { perror(v); return 1; }
            fclose(f);
            v8disk_set_drive2(host_d2_size, host_d2_io, true);
            i++;
        }
#endif
        else if (!strcmp(a, "--tap") && v) {
            if (host_ntaps < 16) host_taps[host_ntaps++] = atol(v);
            i++;
        }
#ifdef HOST_TRACE
        else if (!strcmp(a, "--trace") && v)     { opt_trace = atol(v); i++; }
        else if (!strcmp(a, "--trace-file") && v) { opt_trace_file = v; i++; }
        else if (!strcmp(a, "--stop-from") && v) { opt_stop_from = atol(v); i++; }
        else if (!strcmp(a, "--profile-from") && v) { prof_from = atol(v); prof_enabled = 1; i++; }
        else if (!strcmp(a, "--profile-to") && v)   { prof_to = atol(v); prof_enabled = 1; if (prof_from < 0) prof_from = 0; i++; }
        else if (!strcmp(a, "--profile-top") && v)  { prof_top = atoi(v); i++; }
        else if (!strcmp(a, "--profile-out") && v)  { prof_out = v; prof_enabled = 1; if (prof_from < 0) prof_from = 0; i++; }
        else if (!strcmp(a, "--watch") && v)     { opt_watch = strtoul(v, NULL, 16) & 0x1FFFFF; i++; }
        else if (!strcmp(a, "--stop-pc") && v) {
            char *e; opt_stop_lo = strtoul(v, &e, 16);
            opt_stop_hi = (*e == '-') ? strtoul(e + 1, NULL, 16) : opt_stop_lo;
            i++;
        }
#endif
        else if (!strcmp(a, "--log") && v) {
            host_log_file = fopen(v, "w");
            if (!host_log_file) { perror(v); return 1; }
            i++;
        }
        else { usage(argv[0]); return 2; }
    }
    t_start_us = real_time_us();
    if (opt_shot_every > 0) mkdir(opt_out, 0755);

    atexit(at_exit);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (opt_disk_rw) disk_ov_pool_kb = 8192;   /* >= any image: never full */
    if (!display_init() || !mac_emu_init()) {
        fprintf(stderr, "H host: init failed\n");
        return 1;
    }
    if (opt_disk_rw && !iwm_disk_writable) {
        fprintf(stderr, "H host: --disk-rw needs DISK_WRITABLE=1\n"); return 1;
    }
    if (opt_cpu) {
        int t = opt_cpu == 68030 ? M68K_CPU_TYPE_68030 :
                opt_cpu == 68020 ? M68K_CPU_TYPE_68020 : M68K_CPU_TYPE_68000;
        m68k_set_cpu_type(t);
        m68k_pulse_reset();
        fprintf(stderr, "H host: CPU overridden to %d, PC=$%08X\n", opt_cpu,
                m68k_get_reg(NULL, M68K_REG_PC));
    }
    mac_emu_run(NULL);   /* never returns; host_frame_hook() exits */
    return 0;
}
