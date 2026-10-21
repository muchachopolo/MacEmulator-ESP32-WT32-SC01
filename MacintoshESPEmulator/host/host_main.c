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
#include "mac_video.h"
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

void host_frame_hook(void)
{
    frames++;
    if (opt_vclock > 0) host_vclock_us = (int64_t)frames * opt_vclock;
    /* Event queue sanity: EvQHdr.qHead/qTail ($014C/$0150) must never
     * point into the vector table ($00-$FF), as the old Classic-style
     * mouse posting at $C0/$D8 did on V8. */
    {
        uint8_t *r = mac_mem_get_ram();
        static int evq_bad;
        if (r && evq_bad < 10) {
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
                if (strcmp(cur, apn) != 0) {
                    fprintf(stderr, "H host: F%ld CurApName \"%s\"\n", frames, cur);
                    strcpy(apn, cur);
                }
            }
            /* Time ($20C, seconds since 1904): should advance 1 per 60 frames */
            if (frames % 3000 == 0)
                fprintf(stderr, "H host: F%ld Time $%08X\n", frames,
                        (unsigned)((r[0x20C] << 24) | (r[0x20D] << 16) | (r[0x20E] << 8) | r[0x20F]));
            if ((h && h < 0x100) || (t && t < 0x100)) {
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
    if (opt_disk_rw && iwm_get_disk_data()) {
        FILE *f = fopen(opt_disk_rw, "wb");
        if (f && fwrite(iwm_get_disk_data(), 1, iwm_get_disk_size(), f) ==
                 iwm_get_disk_size()) {
            fprintf(stderr, "H host: wrote %u-byte disk image to %s\n",
                    (unsigned)iwm_get_disk_size(), opt_disk_rw);
        } else {
            perror(opt_disk_rw);
        }
        if (f) fclose(f);
    }
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
        "  --disk-rw FILE  (LC II build) mount the embedded disk writable and save it to\n"
        "                  FILE at exit (desktop-database prep, see docs/build-guide.md)\n"
        "  --cpu 68000|68020|68030  override the CPU type mac_emu_init() set (re-pulses reset)\n"
        "  --tap F                  scripted touch tap starting at frame F (repeatable, max 16)\n"
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

    if (opt_disk_rw) iwm_disk_writable = true;
    if (!display_init() || !mac_emu_init()) {
        fprintf(stderr, "H host: init failed\n");
        return 1;
    }
    if (opt_disk_rw && iwm_get_disk_data()) {
        /* the image is embedded in .rodata: make its pages writable */
        long pg = sysconf(_SC_PAGESIZE);
        uintptr_t lo = (uintptr_t)iwm_get_disk_data() & ~(uintptr_t)(pg - 1);
        uintptr_t hi = (uintptr_t)iwm_get_disk_data() + iwm_get_disk_size();
        if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE) != 0) {
            perror("mprotect"); return 1;
        }
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
