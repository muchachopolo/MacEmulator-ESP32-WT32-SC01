/*
 * Macintosh Classic Emulator - Main emulation loop
 *
 * Orchestrates CPU execution, video rendering, input handling,
 * and interrupt generation.
 */
#include "mac_emulator.h"
#include "mac_memory.h"
#include "mac_video.h"
#include "mac_via.h"
#include "mac_scc.h"
#include "mac_iwm.h"
#include "mac_disk_overlay.h"   /* writable disk */
#include "mac_scsi.h"
#if MAC_ROM_LC_II
#include "mac_v8video.h"
#include "mac_v8disk.h"
#include "mac_egret.h"
#endif
#include "mac_asc.h"
#include "mac_touch.h"
#include "mac_kbd.h"
#include "m68k.h"

#include <string.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_cpu.h>
#include "display.h"

static const char *TAG = "mac_emu";

/* ROM and disk images. MAC_EXTERNAL_IMAGES (mac_emulator.h): the platform
 * loads them at run time (Raspberry Pi: from the SD card) and sets
 * mac_ext_rom/mac_ext_disk before mac_emu_init(). */
#if MAC_EXTERNAL_IMAGES
const uint8_t *mac_ext_rom;
uint32_t mac_ext_rom_size;
uint8_t *mac_ext_disk;
uint32_t mac_ext_disk_size;
#define rom_start  mac_ext_rom
#define rom_end    (mac_ext_rom + mac_ext_rom_size)
#define disk_start mac_ext_disk
#define disk_end   (mac_ext_disk + mac_ext_disk_size)
/* Embedded ROM image (linked from flash via CMake EMBED_FILES) */
#elif MAC_ROM_LC_II && MAC_V8_CUDA
extern const uint8_t rom_start[] asm("_binary_Color_Classic_ROM_start");
extern const uint8_t rom_end[]   asm("_binary_Color_Classic_ROM_end");
#elif MAC_ROM_LC_II
extern const uint8_t rom_start[] asm("_binary_LC_II_ROM_start");
extern const uint8_t rom_end[]   asm("_binary_LC_II_ROM_end");
#else
extern const uint8_t rom_start[] asm("_binary_Classic_ROM_start");
extern const uint8_t rom_end[]   asm("_binary_Classic_ROM_end");
#endif

/* Disk image - only available if HAS_DISK_IMAGE is set in CMakeLists.txt */
#if HAS_DISK_IMAGE && !MAC_EXTERNAL_IMAGES
/* DISK_SYM = C identifier of the embedded file name (CMake/Makefile
 * DISK_NAME, e.g. OS_750_boot_dsk or custom_753_dsk). */
#ifndef DISK_SYM
#define DISK_SYM OS_750_boot_dsk
#endif
#define DISK_STR2(x) #x
#define DISK_STR(x)  DISK_STR2(x)
extern const uint8_t disk_start[] asm("_binary_" DISK_STR(DISK_SYM) "_start");
extern const uint8_t disk_end[]   asm("_binary_" DISK_STR(DISK_SYM) "_end");
#endif

/* Mouse state from touch input */
static volatile int mouse_x = MAC_SCREEN_WIDTH / 2;
static volatile int mouse_y = MAC_SCREEN_HEIGHT / 2;
static volatile bool mouse_button = false;

/* ==== Classic mouse-button event posting (touch -> Mac event queue) ====
 * On the Mac Classic (ADB) GetNextEvent/WaitNextEvent read clicks from the
 * OS event queue (EvQHdr $014A), not from MBState, and the emulator has no
 * ADB mouse whose driver would post them. So the emulator posts them.
 *
 * CLASSIC_MOUSE_POSTEVENT=1 (default): do what the ROM's ADB mouse handler
 * ($403A54) does on a button change -- RndSeed += Ticks-MBTicks, MBTicks =
 * Ticks, MBState = $00/$80, then _PostEvent -- with PostEvent ($403EE6) and
 * Enqueue ($402AB6) mirrored in C: honour SysEvtMask, take a free element
 * of the OS's own event buffer (SysEvtBuf/SysEvtCnt, free = evtQWhat $FFFF;
 * GetOSEvent/FlushEvents mark removed elements that way), else steal the
 * oldest queued element, fill when=Ticks, where=Mouse, modifiers from
 * KeyMap/MBState, qType=evType(4), append. No foreign elements enter the
 * queue, so the OS's own free/steal logic stays consistent.
 * CLASSIC_MOUSE_POSTEVENT=0: the old fixed EvQEl buffers at $C0/$D8 (in the
 * 68010-reserved vector area), never re-enqueued while still linked. They
 * are foreign to SysEvtBuf: when the buffer is full PostEvent steals the
 * queue head, and if that is $C0/$D8 while a new post re-links the same
 * element, the queue gets a qLink self-cycle.
 *
 * Both: posts are made only while the 68k's interrupt mask is 0, i.e. at a
 * point where a real ADB (level 1) interrupt could post. Every queue
 * operation in the OS runs with the mask raised (ORI #$0300), so this never
 * edits the queue in the middle of Enqueue/Dequeue/PostEvent/GetOSEvent.
 * Transitions wait in a small FIFO (normally < 1 frame). */
#ifndef CLASSIC_MOUSE_POSTEVENT
#define CLASSIC_MOUSE_POSTEVENT 1
#endif

#define EVT_BUF_DOWN   0x00C0   /* legacy EvQEl for mouseDown (22 bytes) */
#define EVT_BUF_UP     0x00D8   /* legacy EvQEl for mouseUp  (22 bytes) */
#define EVTQ_HEAD      0x014C   /* EvQHdr.qHead (long) */
#define EVTQ_TAIL      0x0150   /* EvQHdr.qTail (long) */
#define TICKS_GLOBAL   0x016A   /* Ticks low-memory global (long) */

static inline uint32_t rd32(const uint8_t *r, uint32_t a)
{
    return ((uint32_t)r[a]<<24)|((uint32_t)r[a+1]<<16)|
           ((uint32_t)r[a+2]<<8)|r[a+3];
}
static inline void wr32(uint8_t *r, uint32_t a, uint32_t v)
{
    r[a]=(v>>24)&0xFF; r[a+1]=(v>>16)&0xFF;
    r[a+2]=(v>>8)&0xFF; r[a+3]=v&0xFF;
}
static inline void wr16(uint8_t *r, uint32_t a, uint16_t v)
{
    r[a]=(v>>8)&0xFF; r[a+1]=v&0xFF;
}

/* ==== Touch "game mode" (TOUCH_GAME_MODE, default 1) ====
 * Full-screen games that draw straight to the screen own no
 * windows. A mouseDown event from them is routed by System 7's Process
 * Manager to the layer under the point -- the Finder's -- which switches the
 * game out. While such an app is in front, a tap sets the button state
 * (MBState $0172, read by _Button; VIA PB3 on the Classic) and posts its
 * mouseDown/mouseUp with evtQWhere in the menu bar (v=1, h=Mouse.h), which
 * the Process Manager leaves to the front app. (A state-only click is not
 * enough: such games may poll GetNextEvent, never _Button, for start and
 * fire.) The cursor position (Mouse/RawMouse/MTemp) is unchanged. Apps with
 * any window (Finder, DAs, dialogs, SimpleText ...) get normal events.
 * Detection: low-memory globals are swapped per process, so at frame end
 * WindowList ($09D6) / CurApName ($0910) describe the CURRENT process. A
 * windowless game in front still yields a few frames to the Finder's
 * background time, so a score is kept per frame (two compares): +1 while a
 * windowless non-Finder app is current, -4 otherwise; game mode when the
 * score >= 60 (about 1 s of the game in front; ~0.25 s to leave). The mode
 * is decided at button-down and kept for the matching button-up. */
#ifndef TOUCH_GAME_MODE
#define TOUCH_GAME_MODE 1
#endif
#if TOUCH_GAME_MODE
static int gm_score;

static void game_mode_track(const uint8_t *r)
{
    if (!r || !mac_post_boot) return;
    int al = r[0x0910];                    /* CurApName */
    int fl = r[0x02E0];                    /* FinderName */
    bool finder = (al == fl && al > 0 && al < 32 &&
                   memcmp(r + 0x0911, r + 0x02E1, al) == 0);
    bool game = (al > 0 && al < 32 && !finder && rd32(r, 0x09D6) == 0);
    if (game) { if (gm_score < 120) gm_score++; }
    else      { gm_score -= 4; if (gm_score < 0) gm_score = 0; }
}

static bool game_mode_active(void)
{
    /* The frame-end sample above depends on which process happens to be
     * current; on the board (idle fast-forward ends frames in the Process
     * Manager's idle loop) it rarely sees the game. Full-screen games also
     * hide the cursor, and CrsrState ($08D0, word, <0 = hidden) is global,
     * not swapped per process, so it is reliable at the moment of the tap. */
    const uint8_t *r = mac_mem_get_ram();
    if (r && mac_post_boot && (int16_t)((r[0x08D0] << 8) | r[0x08D1]) < 0) return true;
    return gm_score >= 60;
}
#endif

/* Event-queue helpers (both builds: the LC II uses them for game-mode
 * clicks only; its normal clicks go through the emulated ADB mouse). */
#define SYS_EVT_MASK   0x0144   /* word */
#define SYS_EVT_BUF    0x0146   /* long: event buffer */
#define SYS_EVT_CNT    0x0154   /* word: buffer elements - 1 */
#define RND_SEED       0x0156
#define MB_TICKS       0x016E
#define MB_STATE       0x0172
#define KEYMAP_MODS    0x017A   /* KeyMap word holding the modifier keys */
#define MOUSE_PT       0x0830
#define EVQEL_SIZE     22
#define EVQ_UNUSED     __attribute__((unused))

static inline uint16_t rd16(const uint8_t *r, uint32_t a)
{
    return (uint16_t)((r[a] << 8) | r[a+1]);
}

/* ==== Classic full view for full-screen games (GAME_FULL_VIEW) ====
 * The Classic runs native 480x320 (CLASSIC_NATIVE_LCD): the LCD shows the
 * top-left 480x320 of the 512x342 frame buffer 1:1. A full-screen game that
 * draws the whole 512x342 (or gets it via GAME_FULL_SCREENBITS) would lose
 * its right 32 columns and bottom 22 rows, so while it runs the renderer
 * shows the whole buffer scaled to 480x320 (mac_full_view, read by
 * mac_video_render on core 0).
 * Decision, once per frame at frame end (core 1):
 *   candidate = game_mode_active() (cursor hidden, or a windowless non-Finder
 *               app in front for ~1 s; see TOUCH_GAME_MODE)
 *   margin    = hash of columns 480-511 (rows 0-319) + rows 320-341, every
 *               4th frame (2.7 KB of frame buffer)
 *   baseline  = the margin hash once it has been stable for 2 s while not a
 *               candidate and not in full view (absorbs leftovers of an
 *               earlier game, the boot-time grey, ...)
 *   enter: candidate for FV_ENTER_FRAMES (0.5 s) AND margin != baseline
 *          (games that keep to the 480x320 screenBits stay crisp 1:1)
 *   leave: not a candidate for FV_LEAVE_FRAMES (1 s)
 * In full view CrsrPin ($0834) is 0,0,342,512, the touch cursor may reach
 * 512x342 and finger movement is scaled by 512/480 x 342/320 so the cursor
 * follows the finger on the scaled picture. Leaving restores CrsrPin and
 * clamps the cursor into 480x320. The emulation core is unchanged: no
 * patch is applied to the game, only the view and the cursor bounds. */
#if GAME_FULL_VIEW && TOUCH_GAME_MODE
#define FV_ENTER_FRAMES  30
#define FV_LEAVE_FRAMES  60
#define FV_STABLE_SAMPLES 30   /* x 4 frames = 2 s */
#endif
static int mac_vis_w = MAC_VIS_WIDTH, mac_vis_h = MAC_VIS_HEIGHT;
#if MAC_ROM_LC_II && V8_GAME_FULL_VIEW
#define V8GFV 1
#else
#define V8GFV 0
#endif
#if (GAME_FULL_VIEW && TOUCH_GAME_MODE) || V8GFV
static int fv_acc_x, fv_acc_y;          /* touch scaling remainders */
#endif
#if GAME_FULL_VIEW && TOUCH_GAME_MODE
static uint8_t fv_saved_pin[8];         /* CrsrPin before full view */

static uint32_t fv_margin_hash(const uint8_t *r)
{
    uint32_t ram_size = mac_mem_get_ram_size();
    uint32_t sb = rd32(r, 0x0824);
    if (sb == 0) sb = ram_size - 0x5900;
    else if (sb >= ram_size) sb &= ram_size - 1;
    if (sb + MAC_SCREEN_SIZE > ram_size) return 0;
    const uint8_t *scr = r + sb;
    uint32_t h = 2166136261u;
    for (int y = 0; y < MAC_VIS_HEIGHT; y++) {
        const uint8_t *p = scr + y * MAC_SCREEN_STRIDE + MAC_VIS_WIDTH / 8;
        for (int i = 0; i < (MAC_SCREEN_WIDTH - MAC_VIS_WIDTH) / 8; i++)
            h = (h ^ p[i]) * 16777619u;
    }
    const uint8_t *p = scr + MAC_VIS_HEIGHT * MAC_SCREEN_STRIDE;
    for (int i = 0; i < (MAC_SCREEN_HEIGHT - MAC_VIS_HEIGHT) * MAC_SCREEN_STRIDE; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static void full_view_set(uint8_t *r, bool on, int frame)
{
    if (on) {
        memcpy(fv_saved_pin, r + 0x0834, 8);
        wr16(r, 0x0834, 0); wr16(r, 0x0836, 0);                 /* top, left */
        wr16(r, 0x0838, MAC_SCREEN_HEIGHT);                     /* bottom */
        wr16(r, 0x083A, MAC_SCREEN_WIDTH);                      /* right */
        mac_vis_w = MAC_SCREEN_WIDTH;
        mac_vis_h = MAC_SCREEN_HEIGHT;
    } else {
        memcpy(r + 0x0834, fv_saved_pin, 8);
        /* never leave a pin larger than the native screen */
        if (rd16(r, 0x0838) > MAC_VIS_HEIGHT || rd16(r, 0x083A) > MAC_VIS_WIDTH) {
            wr16(r, 0x0834, 0); wr16(r, 0x0836, 0);
            wr16(r, 0x0838, MAC_VIS_HEIGHT); wr16(r, 0x083A, MAC_VIS_WIDTH);
        }
        mac_vis_w = MAC_VIS_WIDTH;
        mac_vis_h = MAC_VIS_HEIGHT;
        mac_emu_set_mouse(mouse_x, mouse_y, mouse_button);    /* clamp */
    }
    fv_acc_x = fv_acc_y = 0;
    mac_full_view = on;
    ESP_LOGI(TAG, "FULL-VIEW %s F%d (CurApName len %d, CrsrState %d, mouse %d,%d)",
             on ? "ON: 512x342 scaled" : "OFF: 480x320 1:1", frame, r[0x0910],
             (int16_t)rd16(r, 0x08D0), mouse_x, mouse_y);
}

static void full_view_track(uint8_t *r, int frame)
{
    static int on_cnt, off_cnt, stable;
    static uint32_t base_h, last_h;
    static bool have_base;
    if (!r || !mac_post_boot) return;
    uint32_t wmgr = rd32(r, 0x09DE);
    if (wmgr == 0xFFFFFFFF || wmgr == 0) return;

    bool cand = game_mode_active();
    if ((frame & 3) == 0) {
        uint32_t h = fv_margin_hash(r);
        if (!cand && !mac_full_view) {
            if (h == last_h) {
                if (stable < FV_STABLE_SAMPLES) stable++;
                else if (!have_base || base_h != h) { base_h = h; have_base = true; }
            } else {
                stable = 0;
            }
        } else {
            stable = 0;
        }
        last_h = h;
    }
    if (cand) { if (on_cnt < FV_ENTER_FRAMES) on_cnt++; off_cnt = 0; }
    else      { if (off_cnt < FV_LEAVE_FRAMES) off_cnt++; on_cnt = 0; }

    if (!mac_full_view) {
        if (on_cnt >= FV_ENTER_FRAMES && have_base && last_h != base_h)
            full_view_set(r, true, frame);
    } else if (off_cnt >= FV_LEAVE_FRAMES) {
        full_view_set(r, false, frame);
    }
}

#endif

#if (GAME_FULL_VIEW && TOUCH_GAME_MODE) || V8GFV
/* Finger movement (LCD pixels) -> Mac pixels on the scaled picture
 * (mac_vis_w x mac_vis_h: 512x342 in the Classic full view, 512 x
 * V8_GAME_FULL_H in the LC II one). */
static void full_view_scale(int *dx, int *dy)
{
    if (!mac_full_view) return;
    fv_acc_x += *dx * mac_vis_w;
    *dx = fv_acc_x / DISPLAY_WIDTH;
    fv_acc_x -= *dx * DISPLAY_WIDTH;
    fv_acc_y += *dy * mac_vis_h;
    *dy = fv_acc_y / DISPLAY_HEIGHT;
    fv_acc_y -= *dy * DISPLAY_HEIGHT;
}
#endif

/* ==== Repaint after a full-screen game quits (MAC_QUIT_REPAINT) ====
 * Old full-screen games (e.g. from the 1980s) draw over the whole screen
 * without a window. When one quits under System 7 nothing invalidates the
 * desktop or the Finder's windows, so the game's last picture stays where
 * no window happens to be redrawn ("black folders"). When an app listed in
 * full_screen_apps[] or in game mode executes _ExitToShell, the next time
 * the Finder calls WaitNextEvent/GetNextEvent the emulator first calls
 * PaintBehind(WindowList, GrayRgn) and then DrawMenuBar, as a JSR whose
 * return address is the event trap itself: the trap then runs unchanged.
 * Entry points come from the toolbox trap table at $0E00 (traps 0-511 on
 * both ROMs). 0 = never repaint (previous behaviour). */
#ifndef MAC_QUIT_REPAINT
#define MAC_QUIT_REPAINT 1
#endif
#if MAC_QUIT_REPAINT
static int qr_stage;            /* 0 idle, 1 PaintBehind due, 2 DrawMenuBar due */

static void qr_aline(unsigned int trap_word)
{
    if ((trap_word & 0xFBFF) != 0xA9F4 || !mac_post_boot) return;     /* _ExitToShell */
    bool game = mac_full_screen_app();
#if TOUCH_GAME_MODE
    game = game || game_mode_active();
#endif
    if (!game) return;
    qr_stage = 1;
    ESP_LOGI(TAG, "QUIT-REPAINT: full-screen app quit, repainting the desktop");
}

/* From the A-line pre-hook (mac_native.c); 1 = handled (no trap now). */
int mac_quit_repaint_pre(unsigned int ir)
{
    if (!qr_stage || (ir != 0xA860 && ir != 0xA970)) return 0;      /* WNE / GNE */
    uint8_t *r = mac_mem_get_ram();
    if (!r) return 0;
    int al = r[0x0910], fl = r[0x02E0];                                  /* Finder current? */
    if (!(al == fl && al > 0 && al < 32 && memcmp(r + 0x0911, r + 0x02E1, al) == 0))
        return 0;
    if (m68k_get_reg(NULL, M68K_REG_SR) & 0x0700) return 0;
    uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
    uint32_t ppc = m68k_get_reg(NULL, M68K_REG_PPC);
    unsigned trap = qr_stage == 1 ? 0x10D : 0x137;                      /* PaintBehind, DrawMenuBar */
    uint32_t entry = rd32(r, 0x0E00 + trap * 4);
    if (qr_stage == 1) {
        sp -= 4; m68k_write_memory_32(sp, rd32(r, 0x09D6));             /* startWindow = WindowList */
        sp -= 4; m68k_write_memory_32(sp, rd32(r, 0x09EE));             /* clobberedRgn = GrayRgn */
    }
    sp -= 4; m68k_write_memory_32(sp, ppc);                             /* return to the event trap */
    m68k_set_reg(M68K_REG_A7, sp);
    m68k_set_reg(M68K_REG_PC, entry);
    qr_stage = qr_stage == 1 ? 2 : 0;
    return 1;
}
#else
int mac_quit_repaint_pre(unsigned int ir) { (void)ir; return 0; }
#endif

#if V8GFV
/* ==== LC II full view for full-screen games (V8_GAME_FULL_VIEW) ====
 * The LC II runs a native 480x320 mode (V8_NATIVE_LCD: the declaration
 * ROM's 512x384 VPBlocks patched), so the main GDevice's gdRect and PixMap
 * bounds are 480x320. Color QuickDraw's InitGraf (LC II $A31530) copies
 * screenBits (baseAddr, rowBytes, bounds) from the main device's PixMap,
 * so a game that centres a fixed 512x342 field on screenBits.bounds draws
 * it 16 px left / 11 rows up and loses the edges.
 * At the A-line hook (before the trap runs), InitGraf ($A86E) from an app
 * in full_screen_apps[] (current process = CurApName) sets both rects to
 * 0,0,V8_GAME_FULL_H,512; the app then gets that screenBits, its ports
 * (OpenCPort copies the device PixMap) and centring. Nothing else in the
 * system changes: ScrnBase / ScreenRow (VRAM, rowBytes 1024, enough for
 * 512 px up to 8 bpp, and 342 or 384 rows of it), GrayRgn, the menu bar
 * and the other processes' QD globals stay as they are. CrsrPin is
 * widened, the renderer shows 512 x V8_GAME_FULL_H scaled to 480x320
 * (mac_full_view) and touch deltas are scaled to match.
 * Restore (GDevice, PixMap, CrsrPin, Mouse clamped, 1:1 view) when that
 * app calls ExitToShell ($A9F4; also the Process Manager's path for an app
 * that died with a system error), or when an unlisted app calls InitGraf.
 * The rects are only written back if they still hold what was set (in
 * case something rebuilt the GDevice meanwhile, e.g. a depth switch).
 * The GDevice is shared: if the Finder comes to the front while the game
 * is still running, it is drawn on the 512-wide device, in the scaled
 * view, until the game quits. */
static bool    v8gfv_on;
static uint8_t v8gfv_saved_pin[8];

/* Main GDevice -> its PixMap (pointers into RAM, 0 if not sane) */
static bool v8gfv_dev(const uint8_t *r, uint32_t *gd, uint32_t *pm)
{
    uint32_t top = mac_mem_get_ram_size();
    uint32_t h = rd32(r, 0x08A4) & 0xFFFFFF;            /* MainDevice */
    if (h < 0x100 || h + 4 > top) return false;
    uint32_t g = rd32(r, h) & 0xFFFFFF;
    if (g < 0x100 || g + 0x3E > top) return false;
    uint32_t ph = rd32(r, g + 0x16) & 0xFFFFFF;         /* gdPMap */
    if (ph < 0x100 || ph + 4 > top) return false;
    uint32_t p = rd32(r, ph) & 0xFFFFFF;
    if (p < 0x100 || p + 0x32 > top) return false;
    *gd = g; *pm = p;
    return true;
}

static inline bool v8gfv_rect_is(const uint8_t *r, uint32_t a, int b, int rt)
{
    return rd32(r, a) == 0 && rd16(r, a + 4) == b && rd16(r, a + 6) == rt;
}

static inline void v8gfv_rect_set(uint8_t *r, uint32_t a, int b, int rt)
{
    wr32(r, a, 0); wr16(r, a + 4, (uint16_t)b); wr16(r, a + 6, (uint16_t)rt);
}

static void v8gfv_set(uint8_t *r, bool on, const char *why)
{
    const int nw = v8video_screen_width(), nh = v8video_screen_height();
    uint32_t gd = 0, pm = 0;
    bool dev = v8gfv_dev(r, &gd, &pm);
    if (on) {
        if (!dev || !v8gfv_rect_is(r, gd + 0x22, nh, nw) || !v8gfv_rect_is(r, pm + 6, nh, nw))
            return;                                     /* not the native device */
        int psize = rd16(r, pm + 0x20);
        uint32_t rb = rd16(r, pm + 4) & 0x3FFF;
        uint32_t base = rd32(r, pm) & 0xFFFFFF;
        if (psize < 1 || psize > 16 || rb < (uint32_t)(V8_GAME_FULL_W * psize / 8) ||
            base < V8_VRAM_BASE ||
            base + (uint32_t)V8_GAME_FULL_H * rb > V8_VRAM_BASE + V8_VRAM_SIZE)
            return;                                     /* 512 px would not fit */
        v8gfv_rect_set(r, gd + 0x22, V8_GAME_FULL_H, V8_GAME_FULL_W);   /* gdRect */
        v8gfv_rect_set(r, pm + 6,    V8_GAME_FULL_H, V8_GAME_FULL_W);   /* bounds */
        memcpy(v8gfv_saved_pin, r + 0x0834, 8);
        v8gfv_rect_set(r, 0x0834, V8_GAME_FULL_H, V8_GAME_FULL_W);      /* CrsrPin */
        mac_vis_w = V8_GAME_FULL_W;
        mac_vis_h = V8_GAME_FULL_H;
    } else {
        if (dev && v8gfv_rect_is(r, gd + 0x22, V8_GAME_FULL_H, V8_GAME_FULL_W))
            v8gfv_rect_set(r, gd + 0x22, nh, nw);
        if (dev && v8gfv_rect_is(r, pm + 6, V8_GAME_FULL_H, V8_GAME_FULL_W))
            v8gfv_rect_set(r, pm + 6, nh, nw);
        memcpy(r + 0x0834, v8gfv_saved_pin, 8);
        if (rd16(r, 0x0838) > nh || rd16(r, 0x083A) > nw)
            v8gfv_rect_set(r, 0x0834, nh, nw);
        mac_vis_w = MAC_VIS_WIDTH;
        mac_vis_h = MAC_VIS_HEIGHT;
        /* clamp the cursor into the native screen */
        int v = (int16_t)rd16(r, 0x0830), h = (int16_t)rd16(r, 0x0832);
        if (v >= nh || h >= nw) {
            if (v >= nh) v = nh - 1;
            if (h >= nw) h = nw - 1;
            uint32_t pt = ((uint32_t)(uint16_t)v << 16) | (uint16_t)h;
            wr32(r, 0x0828, pt); wr32(r, 0x082C, pt); wr32(r, 0x0830, pt);
            if (r[0x08CF]) r[0x08CE] = 0xFF;
        }
    }
    v8gfv_on = on;
    fv_acc_x = fv_acc_y = 0;
    mac_full_view = on;
    ESP_LOGI(TAG, "V8 GAME-VIEW %s (%s; CurApName len %d, depth %d bpp, GDevice $%06X PixMap $%06X)",
             on ? "ON: main GDevice 512-wide, scaled view" : "OFF: 480x320 1:1",
             why, r[0x0910], 1 << v8video_depth_log2(), (unsigned)gd, (unsigned)pm);
}

/* From mac_aline_hook(): every A-line trap, before it runs. */
static inline void v8gfv_aline(unsigned int trap_word)
{
    unsigned t = trap_word & 0xFBFF;                    /* drop autopop bit */
    if (t != 0xA86E && t != 0xA9F4) return;             /* InitGraf, ExitToShell */
    if (!mac_post_boot) return;
    uint8_t *r = mac_mem_get_ram();
    if (!r) return;
    bool listed = mac_full_screen_app();
    if (t == 0xA86E) {
        if (listed && !v8gfv_on)      v8gfv_set(r, true, "InitGraf");
        else if (!listed && v8gfv_on) v8gfv_set(r, false, "InitGraf of another app");
    } else if (listed && v8gfv_on) {
        v8gfv_set(r, false, "ExitToShell");
    }
}
#endif

#if !MAC_ROM_LC_II && !CLASSIC_MOUSE_POSTEVENT
/* Check if a given address is currently in the active event queue */
static bool evt_in_queue(const uint8_t *r, uint32_t target)
{
    uint32_t top = mac_mem_get_ram_size();
    uint32_t addr = rd32(r, EVTQ_HEAD);
    for (int i = 0; i < 64 && addr != 0; i++) {
        if (addr == target) return true;
        if (addr >= top) break;          /* invalid pointer */
        addr = rd32(r, addr);         /* follow qLink */
    }
    return false;
}
#endif

/* ROM Enqueue ($402AB6 Classic) on EvQHdr */
static EVQ_UNUSED void evq_append(uint8_t *r, uint32_t el)
{
    uint32_t top = mac_mem_get_ram_size();
    wr32(r, el, 0);                          /* qLink = NIL */
    uint32_t tail = rd32(r, EVTQ_TAIL);
    if (rd32(r, EVTQ_HEAD) == 0 || tail == 0 || tail >= top) {
        wr32(r, EVTQ_HEAD, el);
        wr32(r, EVTQ_TAIL, el);
    } else {
        wr32(r, tail, el);                   /* old tail.qLink -> el */
        wr32(r, EVTQ_TAIL, el);
    }
}

/* ROM PostEvent (Classic $403EE6; same code in the LC II ROM) for an event
 * with message 'msg', posted at 'where' (a Mac Point, v<<16|h).
 * Returns 0 = posted, 1 = masked by SysEvtMask, -1 = no element. */
static EVQ_UNUSED int mac_post_event_rom(uint8_t *r, int what, uint32_t where, uint32_t msg)
{
    uint32_t top = mac_mem_get_ram_size();
    if (!(rd16(r, SYS_EVT_MASK) & (1u << what))) return 1;   /* evtNotEnb */
    uint32_t buf = rd32(r, SYS_EVT_BUF) & 0xFFFFFF;
    int cnt = rd16(r, SYS_EVT_CNT);
    if (buf < 0x100 || cnt > 256 || buf + (cnt + 1) * EVQEL_SIZE > top)
        return -1;
    uint32_t el = 0;
    for (int i = 0; i <= cnt; i++) {                 /* DBEQ: cnt+1 elements */
        uint32_t e = buf + i * EVQEL_SIZE;
        if (rd16(r, e + 6) == 0xFFFF) { el = e; break; }
    }
    if (!el) {                       /* all in use: recycle the oldest event */
        el = rd32(r, EVTQ_HEAD);
        if (el == 0 || el >= top) return -1;
        wr32(r, EVTQ_HEAD, rd32(r, el));
    }
    uint16_t km = rd16(r, KEYMAP_MODS);
    wr16(r, el + 6,  (uint16_t)what);                /* evtQWhat */
    wr32(r, el + 8,  msg);                           /* evtQMessage */
    wr32(r, el + 12, rd32(r, TICKS_GLOBAL));         /* evtQWhen */
    wr32(r, el + 16, where);                         /* evtQWhere */
    r[el + 20] = (uint8_t)((km << 1) | (km >> 15));  /* ROL.W #1, low byte */
    r[el + 21] = r[MB_STATE];                        /* btnState */
    wr16(r, el + 4, 4);                              /* qType = evType */
    evq_append(r, el);
    return 0;
}

/* Post a mouseDown (down) or mouseUp event, as the ROM's ADB mouse handler
 * (Classic $403A54) does: RndSeed, MBTicks, MBState, then PostEvent at
 * Mouse. A game-mode click (TOUCH_GAME_MODE) is posted at the menu bar
 * (v=1, h=Mouse.h): the Process Manager's layer check (FindWindow ->
 * inMenuBar) then leaves it to the front app instead of switching to the
 * Finder, whose layer owns every point outside the app's (absent) windows. */
static EVQ_UNUSED void mac_post_mouse_event(uint8_t *r, bool down, int mx, int my, bool game)
{
    (void)mx; (void)my;
#if !MAC_ROM_LC_II && !CLASSIC_MOUSE_POSTEVENT
    if (!game) {
        uint32_t buf = down ? EVT_BUF_DOWN : EVT_BUF_UP;
        /* Never re-link an element that is still in the queue (qLink cycle) */
        if (evt_in_queue(r, buf)) return;
        if (!(rd16(r, SYS_EVT_MASK) & (1u << (down ? 1 : 2)))) return;
        wr16(r, buf + 4,  4);              /* qType = evType */
        wr16(r, buf + 6,  down ? 1 : 2);   /* evtQWhat */
        wr32(r, buf + 8,  0);              /* evtQMessage */
        wr32(r, buf + 12, rd32(r, TICKS_GLOBAL)); /* evtQWhen */
        wr16(r, buf + 16, (uint16_t)my);   /* evtQWhere.v */
        wr16(r, buf + 18, (uint16_t)mx);   /* evtQWhere.h */
        wr16(r, buf + 20, down ? 0x0000 : 0x0080); /* evtQModifiers.btnState */
        evq_append(r, buf);
        return;
    }
#endif
    uint32_t t = rd32(r, TICKS_GLOBAL);
    wr32(r, RND_SEED, rd32(r, RND_SEED) + (t - rd32(r, MB_TICKS)));
    wr32(r, MB_TICKS, t);
    r[MB_STATE] = down ? 0x00 : 0x80;
    uint32_t where = rd32(r, MOUSE_PT);
    if (game) {
#if V8GFV
        /* LC II full view: the cursor can reach h 480-511, but the menu bar
         * is the native 480 wide; a click posted at h >= 480 misses it and
         * the Process Manager switches to the Finder (seen on the host). */
        int h = (int16_t)(where & 0xFFFF);
        if (h >= v8video_screen_width()) h = v8video_screen_width() - 1;
        if (h < 0) h = 0;
        where = (1u << 16) | (uint16_t)h;
#else
        where = (1u << 16) | (where & 0xFFFF);
#endif
    }
    int rc = mac_post_event_rom(r, down ? 1 : 2, where, 0);
    if (rc < 0) {
        static int fail_log;
        if (fail_log++ < 5)
            ESP_LOGW(TAG, "MOUSE-POST: no event element (SysEvtBuf=$%06lX)",
                     (unsigned long)rd32(r, SYS_EVT_BUF));
    }
}

#if MAC_KEYBOARD && !MAC_ROM_LC_II
/* Keyboard events (mac_kbd.c), posted at Mouse like the ROM's handler. */
int mac_post_event_msg(uint8_t *r, int what, uint32_t message)
{
    return mac_post_event_rom(r, what, rd32(r, MOUSE_PT), message);
}
#endif

/* Button transitions waiting for a point where the 68k could take an
 * interrupt (SR mask 0). After 30 frames without one they are posted
 * anyway (a click is better than none; the OS never masks that long while
 * editing the queue). Entry: bit 0 = down, bit 1 = game-mode click. */
static uint8_t mb_pend[8];
static int mb_pend_n, mb_pend_wait;

static EVQ_UNUSED void mouse_btn_queue(bool down, bool game)
{
    if (mb_pend_n < (int)sizeof(mb_pend))
        mb_pend[mb_pend_n++] = (uint8_t)((down ? 1 : 0) | (game ? 2 : 0));
}

static EVQ_UNUSED void mouse_btn_flush(uint8_t *r, int mx, int my)
{
    if (mb_pend_n == 0) return;
    if ((m68k_get_reg(NULL, M68K_REG_SR) & 0x0700) && ++mb_pend_wait < 30)
        return;
    for (int i = 0; i < mb_pend_n; i++)
        mac_post_mouse_event(r, mb_pend[i] & 1, mx, my, (mb_pend[i] & 2) != 0);
    mb_pend_n = 0;
    mb_pend_wait = 0;
}

#if MAC_ROM_LC_II
/* V8 touch -> cursor. Position is absolute: the movement is added to the
 * Mac's current Mouse ($0830) and written to MTemp/RawMouse/Mouse with
 * CrsrNew set, like the Classic build. MTemp == RawMouse means the cursor
 * VBL task sees a zero raw delta, so no acceleration is applied and the
 * cursor moves exactly 1:1 with the finger. The emulated ADB mouse
 * (Egret/Cuda HLE) carries the BUTTON only (zero deltas), so clicks go
 * through the OS's own ADB driver and event system.
 * Movement that follows a button transition waits until that transition
 * has been packed into an ADB packet, so the click lands where the finger
 * was when it happened. Only position changes are written (CrsrNew
 * CRITICAL note in CLAUDE.md), and only once the Window Manager is up. */
static void v8_apply_touch(const touch_report_t *tr, int frame)
{
    static int pend_dx, pend_dy, wait_frames;
    uint8_t *r = mac_mem_get_ram();

    pend_dx += tr->dx;
    pend_dy += tr->dy;
    if (pend_dx || pend_dy) {
        uint32_t wmgr = r ? rd32(r, 0x09DE) : 0xFFFFFFFF;
        if (!mac_post_boot || wmgr == 0xFFFFFFFF || wmgr == 0) {
            pend_dx = pend_dy = 0;          /* no cursor yet: drop */
        } else if (egret_adb_mouse_btn_pending() && ++wait_frames < 30) {
            /* let the queued click reach the Mac first */
        } else {
            int v = (int16_t)((r[0x0830] << 8) | r[0x0831]);
            int h = (int16_t)((r[0x0832] << 8) | r[0x0833]);
            int sw = v8video_screen_width(), shh = v8video_screen_height();
#if V8GFV
            if (mac_full_view) { sw = mac_vis_w; shh = mac_vis_h; }
#endif
            h += pend_dx;
            v += pend_dy;
            if (h < 0) h = 0; else if (h >= sw) h = sw - 1;
            if (v < 0) v = 0; else if (v >= shh) v = shh - 1;
            uint32_t pt = ((uint32_t)(uint16_t)v << 16) | (uint16_t)h;
            wr32(r, 0x0828, pt);            /* MTemp */
            wr32(r, 0x082C, pt);            /* RawMouse */
            wr32(r, 0x0830, pt);            /* Mouse */
            if (r[0x08CF]) r[0x08CE] = 0xFF; /* CrsrNew if CrsrCouple */
            pend_dx = pend_dy = 0;
            wait_frames = 0;
        }
    }
    /* Game mode (see TOUCH_GAME_MODE): the ADB mouse keeps its button up
     * (its handler would post at Mouse); the click is posted at the menu
     * bar instead, MBState included (mac_post_mouse_event). */
    static bool gm_click;
    if (tr->btn_changed && r) {
#if TOUCH_GAME_MODE
        if (tr->button) gm_click = game_mode_active();
#endif
        if (gm_click) mouse_btn_queue(tr->button, true);
    }
    if (r) mouse_btn_flush(r, 0, 0);
    egret_adb_mouse(tr->adb_dx, tr->adb_dy, gm_click ? false : tr->button);
    if (tr->btn_changed) {
        static int blog;
        if (r && blog < 60) {
            blog++;
            ESP_LOGI(TAG, "TOUCH-BTN F%d %s%s at Mouse=(%d,%d)", frame,
                     tr->button ? "DOWN" : "UP", gm_click ? " (game mode)" : "",
                     (int16_t)((r[0x0832] << 8) | r[0x0833]),
                     (int16_t)((r[0x0830] << 8) | r[0x0831]));
        }
    }
}
#endif

bool mac_emu_init(void)
{
    ESP_LOGI(TAG, "Initializing Macintosh Classic emulator...");

    /* Calculate ROM size from embedded symbols */
    uint32_t rom_size = (uint32_t)(rom_end - rom_start);
    ESP_LOGI(TAG, "ROM size: %lu bytes", (unsigned long)rom_size);

    if (rom_size < 256) {
        ESP_LOGE(TAG, "ROM image too small or not found");
        return false;
    }

    /* Disk image: use directly from flash (read-only) to save ~1.44MB PSRAM
     * for more Mac RAM. System 7.5 needs 4MB+ RAM; saving PSRAM here lets
     * us allocate 3MB instead of 2MB.
     * Note: the disk is passed as non-const to iwm_init, but boot only reads.
     * Cast away const — IWM write support is a future enhancement. */
    uint8_t *disk_data = NULL;
    uint32_t disk_size = 0;
#if HAS_DISK_IMAGE
    disk_size = (uint32_t)(disk_end - disk_start);
    disk_data = (uint8_t *)(uintptr_t)disk_start;  /* Read-only from flash */
    ESP_LOGI(TAG, "Disk image in flash: %lu bytes (read-only, saving PSRAM for RAM)",
             (unsigned long)disk_size);
#else
    ESP_LOGW(TAG, "No disk image embedded - will show '?' floppy icon");
#endif

    /* Initialize memory subsystem (RAM, ROM, peripherals) */
    ESP_LOGI(TAG, "Free PSRAM before mem init: %lu bytes",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    if (!mac_mem_init(rom_start, rom_size, disk_data, disk_size)) {
        ESP_LOGE(TAG, "Memory initialization failed");
        return false;
    }

    /* Initialize video subsystem */
    if (!mac_video_init()) {
        ESP_LOGE(TAG, "Video initialization failed");
        return false;
    }

    /* Initialize Musashi CPU. The Color Classic is a 68030: its reset code
     * opens with a cache-control op ($F4AC) that faults on a 68000. */
    m68k_init();
#if MAC_ROM_LC_II
    m68k_set_cpu_type(M68K_CPU_TYPE_68030);
#else
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
#endif
    m68k_pulse_reset();

    ESP_LOGI(TAG, "CPU initialized, PC=0x%08X, SP=0x%08X",
             m68k_get_reg(NULL, M68K_REG_PC),
             m68k_get_reg(NULL, M68K_REG_SP));

    return true;
}

/* ------------------------------------------------------------------ */
/* Idle fast-forward (MAC_IDLE_FF, see mac_emulator.h)                 */
/*                                                                     */
/* Two detectors, both off the hot path:                               */
/*  1. Process Manager idle loop (System 7.x, RAM; LC II ~$2DC56,      */
/*     Classic ~$214C0): the scheduler finds no runnable process and   */
/*     executes   _Trap $ABF7 ; MOVE.L A4,D0 ; BEQ.W loop   (bytes     */
/*     ABF7 200C 6700 FF64). Seen from mac_aline_hook() (A-line        */
/*     exception path only). A pass is ~390 instructions (OSEventAvail,*/
/*     $ABF7 -> PPC $A0DD) and its result can only change through an   */
/*     interrupt (VBL -> Ticks, ADB/Egret -> events, Time Manager) or  */
/*     the frame-end touch/disk updates. So when a whole pass ran with  */
/*     neither (mac_idle_epoch unchanged since the previous $ABF7), the */
/*     next pass would be identical: end the slice and fast-forward.    */
/*  2. Tick-wait loops  CMP.L ($016A).W,Dn ; Bcc.S *-4  (ROM DragGrayRgn*/
/*     LC II $A193E8, Classic $40C5C2; Delay etc.): checked on the PC  */
/*     at the end of each slice. The loop only exits when an interrupt */
/*     changes Ticks.                                                   */
/* Fast-forward = advance device time (VIA/Egret, VBL) in fast_slice    */
/* chunks, exactly as if the CPU had spun, until an interrupt above the */
/* SR mask is pending or the frame budget is used up.                   */
/* ------------------------------------------------------------------ */
#if MAC_IDLE_FF
static uint32_t mac_idle_epoch;         /* bumped by real interrupts + frame ends */
static uint32_t ff_abf7_mark = 0xFFFFFFFF;
static volatile bool ff_request;        /* set by mac_aline_hook */
static bool ff_resume_armed;            /* CPU parked at ff_park_pc after an FF */
static uint32_t ff_park_pc;
static uint32_t ff_stat_skipped;        /* CPU cycles skipped (SPEED report) */
static uint32_t ff_stat_count, ff_stat_abf7, ff_stat_tick;

static inline uint16_t ff_peek16(uint32_t a)
{
    a &= 0xFFFFFF;
    const uint8_t *r = mac_mem_get_ram();
    uint32_t rs = mac_mem_get_ram_size();
    if (r && a + 1 < rs) return (uint16_t)((r[a] << 8) | r[a + 1]);
#if MAC_ROM_LC_II
    uint32_t rb = 0xA00000;
#else
    uint32_t rb = 0x400000;
#endif
    uint32_t rsz = (uint32_t)(rom_end - rom_start);
    if (a >= rb && a + 1 < rb + rsz)
        return (uint16_t)((rom_start[a - rb] << 8) | rom_start[a - rb + 1]);
    return 0;
}

/* Bcc.S *-4 (any condition except BRA/BSR), i.e. back to the CMP.L */
static inline bool ff_is_bcc_back(uint16_t w)
{
    return (w & 0xF0FF) == 0x60FA && (w & 0x0E00) != 0;
}

/* PC sits in "CMP.L ($016A).W,Dn ; Bcc.S *-4" */
static bool ff_ticks_wait_at(uint32_t pc)
{
    uint16_t w = ff_peek16(pc);
    if ((w & 0xF1FF) == 0xB0B8)
        return ff_peek16(pc + 2) == 0x016A && ff_is_bcc_back(ff_peek16(pc + 4));
    if (ff_is_bcc_back(w))
        return (ff_peek16(pc - 4) & 0xF1FF) == 0xB0B8 && ff_peek16(pc - 2) == 0x016A;
    return false;
}

void mac_aline_hook(unsigned int trap_word, unsigned int trap_pc)
{
#if V8GFV
    v8gfv_aline(trap_word);
#endif
#if MAC_QUIT_REPAINT
    qr_aline(trap_word);
#endif
    ff_resume_armed = false;            /* the CPU has left the park point */
    if (trap_word != 0xABF7 || !mac_post_boot) return;
    trap_pc &= 0xFFFFFF;
    if (ff_peek16(trap_pc + 2) != 0x200C || ff_peek16(trap_pc + 4) != 0x6700 ||
        ff_peek16(trap_pc + 6) != 0xFF64)
        return;                         /* $ABF7 from SystemTask etc.: not idle */
    if (ff_abf7_mark == mac_idle_epoch) {
        ff_request = true;
        ff_stat_abf7++;
        m68k_end_timeslice();
    }
    ff_abf7_mark = mac_idle_epoch;
}

/* Called from the interrupt-acknowledge callback. */
void mac_idle_note_irq(void)
{
    /* Interrupts taken where an FF parked the CPU happen before the next
     * idle pass starts, so that pass already sees their effects. */
    if (ff_resume_armed &&
        (m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF) == ff_park_pc)
        return;
#if MAC_ROM_LC_II
    if (!via_irq_pending() && !v8video_irq_pending()) return;
#else
    if (!via_irq_pending()) return;     /* Classic: spurious re-takes of a cleared line */
#endif
    mac_idle_epoch++;
}

/* Highest interrupt level currently requested by the devices */
static inline int ff_pending_level(void)
{
#if MAC_ROM_LC_II
    if (v8video_irq_pending()) return 2;
#endif
    return via_irq_pending() ? 1 : 0;
}
#else
void mac_aline_hook(unsigned int trap_word, unsigned int trap_pc)
{
    (void)trap_word; (void)trap_pc;
#if V8GFV
    v8gfv_aline(trap_word);
#endif
#if MAC_QUIT_REPAINT
    qr_aline(trap_word);
#endif
}
void mac_idle_note_irq(void) {}
#endif /* MAC_IDLE_FF */

void mac_emu_run(void *param)
{
    (void)param;

    ESP_LOGI(TAG, "Starting emulation loop on core %d", xPortGetCoreID());

    /* No frame rate limiting — run at maximum speed.
     * Video rendering is on core 0 (mac_video_task). */

    /* How many CPU cycles to run per time slice before checking peripherals.
     * During boot (first 50 frames), use tiny slices (1 cycle = 1 instruction)
     * so we can intercept the exact memtest check at 0x401CF0.
     * When disk reads are active, use small slices (10 cycles ≈ 2-3 instructions)
     * so PC traps can catch .SONY driver flow.
     * After boot (F700+), use larger slices to minimize loop overhead.
     * fast_slice must stay ≤ 4000: the VIA T2 timer often has short periods
     * (100-2000 cycles), and coarser ticks cause timer underflow misses that
     * corrupt interrupt timing — this was the root cause of the F8000 crash
     * (PC=$C80584, SP=$000000) seen with fast_slice=40000. */
    /* In CPU cycles; the device-time granularity (what the VIA limit above
     * is about) stays 1000/4000 device cycles at any MAC_CYCLE_SCALE. */
    const int normal_slice = 1000 / MAC_CYCLE_SCALE;
    const int fast_slice = 4000 / MAC_CYCLE_SCALE;
    const int disk_slice = 10;

    int frame_count = 0;
    bool boot_logged = false;
    int dispatch_frames = 0;
    int iwm_poll_frames = 0;
    int iwm_bypass_count = 0;
    bool read_entry_seen = false;

    /* Trap ring buffer: captures last 32 A-line trap words before a crash.
     * Populated when PC hits the Line-A handler; dumped on invalid PC. */
    static uint32_t line_a_handler_addr = 0;
    static uint16_t trap_ring[32];
    static uint32_t trap_caller_ring[32];
    static int trap_ring_idx = 0;
    static bool crash_recovered = false;

    /* Profiling: measure where frame time goes (CPU exec vs overhead) */
    uint64_t prof_exec_sum = 0, prof_total_sum = 0;
    int prof_frame_count = 0;

    /* PC histogram: 4KB pages, buckets 0-511 RAM, 512-639 ROM */
    static uint32_t pc_hist[640];
    static uint32_t pc_bad_count = 0;
    static int pc_samples = 0;
    static int hist_report_at = 600;
#if MAC_ROM_LC_II
    static uint32_t v8_pk[16], v8_pv[16];
    static uint32_t v8_ring[32];
    static uint32_t v8_ri;
    static uint32_t v8_page[512];
    static uint32_t v8_ps_cum;
    static bool v8_cleared;
#endif

    while (1) {
        /* Execute one frame's worth of CPU cycles */
        int cycles_remaining = MAC_CPU_CYCLES_PER_FRAME;
        int total_executed = 0;
        bool vblank_fired = false;
#if MAC_ROM_LC_II
        v8video_vbl(false);   /* built-in video VBL line inactive until mid-frame */
#endif
        uint32_t frame_cstart = esp_cpu_get_cycle_count();

        while (cycles_remaining > 0) {
            /* Slice sizing:
             * F0-80: single-step for memory test bypass
             * F600-640: single-step for trap ring buffer
             * Disk reads: small slices for driver flow intercepts
             * F700-F2000: normal slices — resource trap monitoring active
             * F2000+: large slices — boot diagnostics done, max throughput */
            int use_slice;
            if (frame_count >= 600 && frame_count <= 640) {
                use_slice = 1;
            } else if (iwm_is_reading()) {
                use_slice = disk_slice;
            } else if (frame_count > 2000) {
                use_slice = fast_slice;
            } else {
                use_slice = normal_slice;
            }
            int slice = (cycles_remaining > use_slice) ? use_slice : cycles_remaining;
            iwm_start_slice(slice);
            uint32_t t_exec0 = esp_cpu_get_cycle_count();
            int executed = m68k_execute(slice);
            uint32_t t_exec1 = esp_cpu_get_cycle_count();
#if MAC_IDLE_FF
            ff_resume_armed = false;
#endif
            prof_exec_sum += (t_exec1 - t_exec0);
            cycles_remaining -= executed;
            total_executed += executed;

            /* Read PC once per slice — shared by all post-execute checks.
             * Eliminates 7 redundant m68k_get_reg calls per slice. */
            uint32_t pc_slice = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            static uint32_t prev_pc_xfer = 0xFFFFFFFF;
            if (frame_count < 600 && (prev_pc_xfer & 0xFFF00000) == 0x00400000 && (pc_slice & 0xFFF00000) != 0x00400000) {
                static int xfer_cnt = 0;
                if (xfer_cnt < 60)
                    ESP_LOGW(TAG, "XFER #%d: $%06X -> $%06X F%d", xfer_cnt++, prev_pc_xfer, pc_slice, frame_count);
            }
            prev_pc_xfer = pc_slice;

            /* === POST-EXECUTE PC VALIDATION ===
             * Catches runaway PC before it cascades into unrecoverable state. */
            {
                static bool crash_detected = false;
                if (!crash_detected) {
                    /* Valid ranges: RAM $0-$9FFFFF minus device windows, ROM
                     * $A00000-$AFFFFF, ROM alias $600000-$60FFFF. */
#if MAC_ROM_LC_II
                    if (pc_slice >= 0xB00000 &&
                        !(pc_slice >= V8DISK_BASE && pc_slice < V8DISK_END)) {  /* .Sony Prime stub */
#else
                    if ((pc_slice >= 0x200000 && pc_slice < 0x400000) ||
                        pc_slice >= 0x480000) {
#endif
                        crash_detected = true;
                        uint32_t sp_chk = m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF;
                        uint32_t sr_chk = m68k_get_reg(NULL, M68K_REG_SR);
                        ESP_LOGE(TAG, "*** CRASH DETECTED F%d: PC=$%06X SP=$%06X SR=%04X ***",
                                 frame_count, pc_slice, sp_chk, sr_chk);
                        ESP_LOGE(TAG, "  D0=$%08X D1=$%08X D2=$%08X D3=$%08X",
                                 m68k_get_reg(NULL, M68K_REG_D0),
                                 m68k_get_reg(NULL, M68K_REG_D1),
                                 m68k_get_reg(NULL, M68K_REG_D2),
                                 m68k_get_reg(NULL, M68K_REG_D3));
                        ESP_LOGE(TAG, "  D4=$%08X D5=$%08X D6=$%08X D7=$%08X",
                                 m68k_get_reg(NULL, M68K_REG_D4),
                                 m68k_get_reg(NULL, M68K_REG_D5),
                                 m68k_get_reg(NULL, M68K_REG_D6),
                                 m68k_get_reg(NULL, M68K_REG_D7));
                        ESP_LOGE(TAG, "  A0=$%06X A1=$%06X A2=$%06X A3=$%06X",
                                 m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF,
                                 m68k_get_reg(NULL, M68K_REG_A1) & 0xFFFFFF,
                                 m68k_get_reg(NULL, M68K_REG_A2) & 0xFFFFFF,
                                 m68k_get_reg(NULL, M68K_REG_A3) & 0xFFFFFF);
                        ESP_LOGE(TAG, "  A4=$%06X A5=$%06X A6=$%06X A7=$%06X",
                                 m68k_get_reg(NULL, M68K_REG_A4) & 0xFFFFFF,
                                 m68k_get_reg(NULL, M68K_REG_A5) & 0xFFFFFF,
                                 m68k_get_reg(NULL, M68K_REG_A6) & 0xFFFFFF,
                                 m68k_get_reg(NULL, M68K_REG_A7) & 0xFFFFFF);
                        /* Stack dump if SP is in valid RAM */
                        uint8_t *cr = mac_mem_get_ram();
                        uint32_t crsz = mac_mem_get_ram_size();
                        if (cr && sp_chk < crsz - 32) {
                            ESP_LOGE(TAG, "  STACK@$%06X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                                sp_chk,
                                cr[sp_chk],cr[sp_chk+1],cr[sp_chk+2],cr[sp_chk+3],
                                cr[sp_chk+4],cr[sp_chk+5],cr[sp_chk+6],cr[sp_chk+7],
                                cr[sp_chk+8],cr[sp_chk+9],cr[sp_chk+10],cr[sp_chk+11],
                                cr[sp_chk+12],cr[sp_chk+13],cr[sp_chk+14],cr[sp_chk+15]);
                        }
                        if (cr && crsz >= 0x100000) {
                            uint32_t pco = pc_slice & (crsz - 1);
                            ESP_LOGE(TAG, "  PCBYTES@%06X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                                pco,
                                cr[pco],cr[pco+1],cr[pco+2],cr[pco+3],
                                cr[pco+4],cr[pco+5],cr[pco+6],cr[pco+7],
                                cr[pco+8],cr[pco+9],cr[pco+10],cr[pco+11],
                                cr[pco+12],cr[pco+13],cr[pco+14],cr[pco+15]);
                        }
#if MAC_ROM_LC_II
                        extern uint32_t v8_fetch_ring[256];
                        extern int v8_fetch_idx;
                        {
                            int first = v8_fetch_idx < 256 ? 0 : (v8_fetch_idx & 255);
                            int n = v8_fetch_idx < 256 ? v8_fetch_idx : 256;
                            ESP_LOGE(TAG, "  FETCH-RING last %d 16-bit bus reads:", n);
                            for (int i = 0; i < n; i += 8) {
                                ESP_LOGE(TAG, "   %06X %06X %06X %06X %06X %06X %06X %06X",
                                    v8_fetch_ring[(first+i+0)&255], v8_fetch_ring[(first+i+1)&255],
                                    v8_fetch_ring[(first+i+2)&255], v8_fetch_ring[(first+i+3)&255],
                                    v8_fetch_ring[(first+i+4)&255], v8_fetch_ring[(first+i+5)&255],
                                    v8_fetch_ring[(first+i+6)&255], v8_fetch_ring[(first+i+7)&255]);
                            }
                        }
#endif
                        /* Trap ring dump */
                        {
                            int n = trap_ring_idx < 32 ? trap_ring_idx : 32;
                            ESP_LOGE(TAG, "  TRAP-RING: last %d of %d:", n, trap_ring_idx);
                            for (int i = 0; i < n; i++) {
                                int idx = ((trap_ring_idx - n + i) % 32 + 32) % 32;
                                ESP_LOGE(TAG, "    T[%d] $%04X from $%06X",
                                         i, trap_ring[idx],
                                         trap_caller_ring[idx] & 0xFFFFFF);
                            }
                        }
                        /* Key low-mem globals */
                        if (cr) {
                            int16_t ds_err = (int16_t)((cr[0xAF0]<<8)|cr[0xAF1]);
                            int16_t mem_err = (int16_t)((cr[0x220]<<8)|cr[0x221]);
                            uint32_t buserr_vec = ((uint32_t)cr[0x08]<<24)|((uint32_t)cr[0x09]<<16)|
                                                  ((uint32_t)cr[0x0A]<<8)|cr[0x0B];
                            uint32_t addrerr_vec = ((uint32_t)cr[0x0C]<<24)|((uint32_t)cr[0x0D]<<16)|
                                                   ((uint32_t)cr[0x0E]<<8)|cr[0x0F];
                            ESP_LOGE(TAG, "  DSErr=%d MemErr=%d BusErrVec=$%06X AddrErrVec=$%06X",
                                     ds_err, mem_err, buserr_vec & 0xFFFFFF, addrerr_vec & 0xFFFFFF);
                        }
                        /* Recovery: redirect to tight loop (Classic only — on V8
                         * it masks the real fault) */
                        if (cr && !MAC_ROM_LC_II) {
                            m68k_set_reg(M68K_REG_SP, 0x0FFA00);
                            m68k_set_reg(M68K_REG_PC, 0x000036);
                            ESP_LOGW(TAG, "CRASH-RECOVERY: PC=$0036 SP=$0FFA00");
                        }
                    }
                }
            }

            /* Fire VBlank mid-frame so ROM dispatch loop can see CA1 in IFR
             * even if it reads ORA (which clears CA1) earlier in the loop */
            if (!vblank_fired && total_executed >= MAC_CPU_CYCLES_PER_FRAME / 2) {
                via_vblank();
#if MAC_ROM_LC_II
                v8video_vbl(true);
#endif
                vblank_fired = true;
            }

            /* === BOOT BYPASS: Force all memory tests to pass ===
             * The ROM has THREE memory test checkpoints:
             * 1. 0x401CF0: TST.L D6 in first-stage boot
             * 2. 0x401D80: TST.L D6 in second-stage boot (called via BSR $401D3E
             *              from 0x400064). If D6!=0 here, ROM jumps to dispatch
             *              loop (serial debug monitor) which has NO timeout.
             * 3. 0x401D96: TST.L D6 in second memory test (0x401D3E)
             * Clear D6 at ALL THREE so the ROM always sees "all tests passed". */
            if (frame_count < 80) {
#if !MAC_ROM_LC_II
                if (pc_slice == 0x401CF0 || pc_slice == 0x401D80 || pc_slice == 0x401D96) {
                    uint32_t old_d6 = m68k_get_reg(NULL, M68K_REG_D6);
                    if (old_d6 != 0) {
                        m68k_set_reg(M68K_REG_D6, 0);
                        ESP_LOGW(TAG, "BYPASS: Cleared D6 at PC=%06X F%d (was 0x%08X)",
                                 pc_slice, frame_count, old_d6);
                    }
                }
#endif
            }

            /* Log when normal boot entry is reached */
            if (!boot_logged) {
                if (pc_slice == 0x401092) {
                    boot_logged = true;
                    ESP_LOGI(TAG, "BOOT: Normal boot entry at F%d", frame_count);
                }
            }

            /* SCSI polling bypass: ROM at 41A44A polls SCSI BSY in a tight loop.
             * With no SCSI device, clear loop counters to exit quickly.
             * Must remain active at ALL times — the ROM's SCSI Manager
             * scans the bus during boot AND INIT loading. Without this
             * bypass, the scan causes heap corruption (crash at F5239). */
            {
#if !MAC_ROM_LC_II
                if (pc_slice == 0x41A44A || pc_slice == 0x41A44E || pc_slice == 0x41A452) {
                    m68k_set_reg(M68K_REG_D1, 0);
                    m68k_set_reg(M68K_REG_D5, 0);
                }
#endif
            }

            /* DM-spin rescue handled at instruction-fetch level in mac_memory.c
             * (read16_dm_spin_rescue at $4031BE). No per-slice check needed. */

            /* Boot _Read bypass: ROM-level NOP at $400CAC handled in
             * mac_memory.c via mac_boot_read_bypass flag. */

            /* I/O completion bypass: the ROM's I/O Manager polls ioResult
             * at $404332 after issuing a synchronous I/O call. Since our
             * SWIM driver can't complete sector reads, fulfill the request
             * directly from the disk image and set ioResult=0. */
            if (mac_boot_read_bypass) {
#if !MAC_ROM_LC_II
                if (pc_slice == 0x404332) {
                    static int io_bypass_count = 0;
                    static int io_poll_count = 0;
                    uint8_t *rram = mac_mem_get_ram();
                    uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF;
                    uint32_t ram_sz = mac_mem_get_ram_size();
                    uint32_t a0m = a0 % ram_sz;
                    if (rram && a0m + 0x32 < ram_sz) {
                        int16_t ioResult = (int16_t)((rram[a0m+0x10]<<8)|rram[a0m+0x11]);
                        int16_t ioRefNum_chk = (int16_t)((rram[a0m+0x18]<<8)|rram[a0m+0x19]);
                        /* Also read ioVRefNum ($16), ioDrvNum ($1C), ioTrap ($06) */
                        int16_t ioVRef = (int16_t)((rram[a0m+0x16]<<8)|rram[a0m+0x17]);
                        uint16_t ioDrv = (rram[a0m+0x1C]<<8)|rram[a0m+0x1D];
                        uint16_t ioTrapP = (rram[a0m+0x06]<<8)|rram[a0m+0x07];
                        io_poll_count++;
                        if (io_poll_count <= 20 || (io_poll_count % 500 == 0)) {
                            ESP_LOGW(TAG, "IO-POLL #%d: res=%d ref=%d vref=%d drv=%d trap=%04X A0=%06X",
                                     io_poll_count, ioResult, ioRefNum_chk,
                                     ioVRef, ioDrv, ioTrapP, a0);
                        }
                        /* When _MountVol returns -56, dump drive queue state */
                        if (ioResult == -56 && io_poll_count <= 3) {
                            uint32_t qh = ((uint32_t)rram[0x30A]<<24)|
                                          ((uint32_t)rram[0x30B]<<16)|
                                          ((uint32_t)rram[0x30C]<<8)|rram[0x30D];
                            ESP_LOGW(TAG, "DQ-CHK: qHead=%08lX dip=%02X inst=%02X sides=%02X",
                                     (unsigned long)qh,
                                     qh ? rram[(qh-3)%ram_sz] : 0xFF,
                                     qh ? rram[(qh-2)%ram_sz] : 0xFF,
                                     qh ? rram[(qh-1)%ram_sz] : 0xFF);
                            if (qh && qh < ram_sz) {
                                uint32_t qm = qh % ram_sz;
                                ESP_LOGW(TAG, "DQ-EL: link=%02X%02X%02X%02X typ=%02X%02X drv=%02X%02X ref=%02X%02X",
                                         rram[qm], rram[qm+1], rram[qm+2], rram[qm+3],
                                         rram[qm+4], rram[qm+5], rram[qm+6], rram[qm+7],
                                         rram[qm+8], rram[qm+9]);
                            }
                        }
                        /* Intercept only when ioResult > 0 (in-progress).
                         * ioResult=1 means the I/O hasn't completed yet.
                         * ioResult=0 means success, negative means error —
                         * both are COMPLETED states that the ROM handles
                         * natively (the poll loop exits on ioResult <= 0).
                         * Previously we intercepted ioResult != 0 which
                         * masked legitimate errors like fnfErr (-43). */
                        if (ioResult > 0) {
                            int16_t ioRefNum = (int16_t)((rram[a0m+0x18]<<8)|rram[a0m+0x19]);
                            uint32_t ioBuffer = ((uint32_t)rram[a0m+0x20]<<24)|
                                ((uint32_t)rram[a0m+0x21]<<16)|
                                ((uint32_t)rram[a0m+0x22]<<8)|rram[a0m+0x23];
                            uint32_t ioReqCount = ((uint32_t)rram[a0m+0x24]<<24)|
                                ((uint32_t)rram[a0m+0x25]<<16)|
                                ((uint32_t)rram[a0m+0x26]<<8)|rram[a0m+0x27];
                            int32_t ioPosOffset = (int32_t)(((uint32_t)rram[a0m+0x2E]<<24)|
                                ((uint32_t)rram[a0m+0x2F]<<16)|
                                ((uint32_t)rram[a0m+0x30]<<8)|rram[a0m+0x31]);
                            uint8_t *disk = iwm_get_disk_data();
                            uint32_t dsize = iwm_get_disk_size();
                            uint32_t buf_m = ioBuffer % ram_sz;

                            /* Only fill for .SONY reads (ioRefNum=-5) */
                            if (ioRefNum == -5 && disk && ioReqCount > 0 &&
                                ioReqCount <= 0x100000 &&
                                buf_m + ioReqCount <= ram_sz &&
                                ioPosOffset >= 0 &&
                                (uint32_t)ioPosOffset + ioReqCount <= dsize) {
                                disk_ov_xfer(rram[a0m+0x07] == 0x03, rram + buf_m, ioPosOffset, ioReqCount); /* writable disk: overlay */
                                rram[a0m+0x28]=rram[a0m+0x24]; rram[a0m+0x29]=rram[a0m+0x25];
                                rram[a0m+0x2A]=rram[a0m+0x26]; rram[a0m+0x2B]=rram[a0m+0x27];
                                /* Clear ioCompletion */
                                rram[a0m+0x0C] = 0; rram[a0m+0x0D] = 0;
                                rram[a0m+0x0E] = 0; rram[a0m+0x0F] = 0;
                                /* Set ioResult=0, D0=0 to signal completion */
                                rram[a0m+0x10] = 0; rram[a0m+0x11] = 0;
                                m68k_set_reg(M68K_REG_D0, 0);
                                if (io_bypass_count < 50) {
                                    uint16_t ioTrap = (rram[a0m+0x06]<<8)|rram[a0m+0x07];
                                    ESP_LOGW(TAG, "IO-DONE #%d: ref=%d buf=$%06X off=%ld cnt=%lu trap=%04X",
                                             io_bypass_count, ioRefNum, ioBuffer & 0xFFFFFF,
                                             (long)ioPosOffset, (unsigned long)ioReqCount,
                                             ioTrap);
                                }
                            } else if (io_bypass_count < 50) {
                                uint16_t ioTrap = (rram[a0m+0x06]<<8)|rram[a0m+0x07];
                                ESP_LOGW(TAG, "IO-PEND #%d: ref=%d buf=$%06X off=%ld cnt=%lu trap=%04X",
                                         io_bypass_count, ioRefNum, ioBuffer & 0xFFFFFF,
                                         (long)ioPosOffset, (unsigned long)ioReqCount,
                                         ioTrap);
                            }
                            io_bypass_count++;
                        }
                    }
                }
#endif
            }

            /* ROM flow PC trace: log key decision points.
             * Skip entirely after F700 — boot is done, these checks
             * are pure overhead (~500 comparisons/frame at slice=1000). */
            if (frame_count <= 700) {
                uint32_t pcd = pc_slice;
                static uint32_t last_trace_pc = 0;
                static int trace_count = 0;
                static int secrd_count = 0;
                /* Track last 4 unique valid PCs for crash backtrace */
                static uint32_t bad_pc_hist[4] = {0};
                static int bad_pc_idx = 0;
                if (pcd != last_trace_pc &&
                    (pcd < 0x200000 || (pcd >= 0x400000 && pcd < 0x480000))) {
                    bad_pc_hist[bad_pc_idx % 4] = pcd;
                    bad_pc_idx++;
                }
                if (pcd != last_trace_pc) {
                    /* === TRAP RING BUFFER ===
                     * Record every A-line trap word in a ring buffer.
                     * When the Line-A handler is entered, the exception frame
                     * on the stack has: SR(2) + saved_PC(4).  The trap word
                     * is at saved_PC (68000 saves PC of the Line-A opcode). */
                    if (pcd == line_a_handler_addr && line_a_handler_addr != 0) {
                        uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF;
                        uint8_t *rram = mac_mem_get_ram();
                        uint32_t rsz = mac_mem_get_ram_size();
                        if (rram && sp < rsz && sp + 5 < rsz) {
                            uint32_t saved_pc = ((uint32_t)rram[sp+2]<<24)|
                                                ((uint32_t)rram[sp+3]<<16)|
                                                ((uint32_t)rram[sp+4]<<8)|rram[sp+5];
                            uint32_t spc = saved_pc & 0xFFFFFF;
                            uint16_t tw = m68k_read_memory_16(spc);
                            int ri = trap_ring_idx % 32;
                            trap_ring[ri] = tw;
                            trap_caller_ring[ri] = spc;
                            trap_ring_idx++;

                            /* === RESOURCE TRAP LOGGING ===
                             * Log parameters for _GetResource ($A9A0),
                             * _Get1Resource ($A9A7), _GetIndResource ($A99C),
                             * _Count1Resources ($A80D).
                             * Pascal calling convention, after exception frame:
                             *   SP+6: theID/index (2 bytes)
                             *   SP+8: theType (4 bytes, e.g. 'lpch') */
                            if (sp + 13 < rsz &&
                                (tw == 0xA9A0 || tw == 0xA9A7 ||
                                 tw == 0xA99C || tw == 0xA80D)) {
                                static int res_log_cnt = 0;
                                int16_t res_id = (int16_t)((rram[sp+6]<<8)|rram[sp+7]);
                                char rtype[5];
                                rtype[0] = rram[sp+8];
                                rtype[1] = rram[sp+9];
                                rtype[2] = rram[sp+10];
                                rtype[3] = rram[sp+11];
                                rtype[4] = '\0';
                                /* Sanitize non-printable chars */
                                for (int ci = 0; ci < 4; ci++)
                                    if (rtype[ci] < 0x20 || rtype[ci] > 0x7E) rtype[ci] = '?';
                                if (res_log_cnt < 200) {
                                    const char *tname =
                                        (tw == 0xA9A0) ? "GetRes" :
                                        (tw == 0xA9A7) ? "Get1Res" :
                                        (tw == 0xA99C) ? "GetIndRes" : "Cnt1Res";
                                    ESP_LOGW(TAG, "RES #%d: %s('%s',%d) PC=$%06X F%d",
                                             res_log_cnt, tname, rtype, res_id,
                                             spc, frame_count);
                                }
                                res_log_cnt++;
                            }

                            /* _OpenResFile ($A997): log the filename being opened.
                             * Stack after Line-A exception:
                             *   SP+0: SR(2), SP+2: PC(4), SP+6: filenamePtr(4), SP+10: result(2)
                             * Pascal: result pushed first, then fileName, then trap. */
                            if (tw == 0xA997 && sp + 15 < rsz) {
                                static int orf_cnt = 0;
                                uint32_t fn_ptr = ((uint32_t)rram[sp+6]<<24)|
                                                   ((uint32_t)rram[sp+7]<<16)|
                                                   ((uint32_t)rram[sp+8]<<8)|rram[sp+9];
                                fn_ptr &= 0xFFFFFF;
                                char fname[64] = {0};
                                if (fn_ptr < rsz) {
                                    int flen = rram[fn_ptr]; /* Pascal string length */
                                    if (flen > 0 && flen < 60 && fn_ptr + flen < rsz) {
                                        for (int fi = 0; fi < flen; fi++)
                                            fname[fi] = rram[fn_ptr + 1 + fi];
                                        fname[flen] = '\0';
                                    }
                                }
                                if (orf_cnt < 20) {
                                    uint32_t a5v = m68k_get_reg(NULL, M68K_REG_A5) & 0xFFFFFF;
                                    ESP_LOGW(TAG, "OPENRF #%d: \"%s\" ptr=$%06X A5=$%06X PC=$%06X F%d",
                                             orf_cnt, fname, fn_ptr, a5v, spc, frame_count);
                                    orf_cnt++;
                                }
                                /* Boot 3 calls _OpenResFile("StartUpScreen").
                                 * If the file doesn't exist, boot 3 at offset $00F8
                                 * checks BTST #2,$0B21 and BNE.S $015A.  We must NOT
                                 * set bit 2 because offset $068A uses the SAME bit to
                                 * skip _InitWindows (meant for when StartUpScreen did
                                 * its own WM init).  Instead, patch the BNE at offset
                                 * $00FE to BRA (unconditional) so the error is skipped
                                 * without touching the flag.  Boot 3 base = spc - $C4. */
                                if (fname[0] == 'S' && fname[1] == 't' && fname[5] == 'U') {
                                    uint32_t boot3_base = (spc - 0xC4) & 0xFFFFFF;
                                    uint32_t bne_addr = boot3_base + 0xFE;
                                    if (bne_addr < rsz && rram[bne_addr] == 0x66) {
                                        rram[bne_addr] = 0x60; /* BNE.S → BRA.S */
                                        ESP_LOGW(TAG, "OPENRF-PATCH: BNE->BRA at $%06X (boot3+$FE) for StartUpScreen skip",
                                                 bne_addr);
                                    } else {
                                        ESP_LOGE(TAG, "OPENRF-PATCH: unexpected byte $%02X at $%06X (expected $66)",
                                                 bne_addr < rsz ? rram[bne_addr] : 0xFF, bne_addr);
                                    }
                                }
                            }

                            /* _SysError ($A9C9): log the error code from D0. */
                            if (tw == 0xA9C9) {
                                uint32_t d0 = m68k_get_reg(NULL, M68K_REG_D0);
                                uint32_t a5v = m68k_get_reg(NULL, M68K_REG_A5) & 0xFFFFFF;
                                static int syserr_cnt = 0;
                                if (syserr_cnt < 10) {
                                    ESP_LOGE(TAG, "SYSERR #%d: code=%d ($%04X) A5=$%06X PC=$%06X F%d",
                                             syserr_cnt, (int16_t)(d0 & 0xFFFF),
                                             (unsigned)(d0 & 0xFFFF), a5v, spc, frame_count);
                                    syserr_cnt++;
                                }
                            }
                        }
                    }

                    /* === SAFE HANDLER HIT LOG ===
                     * When PC=$001800 (our safe handler), an unimplemented
                     * trap was called.  Log the trap word by reading 2 bytes
                     * before the return address on the stack. */
                    if (pcd == 0x000030 && frame_count >= 600) {
                        static int safe_cnt = 0;
                        static uint16_t trap_hist[16];
                        static int trap_hist_cnt[16];
                        uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF;
                        uint8_t *rram = mac_mem_get_ram();
                        uint32_t ret_addr = 0;
                        uint16_t trap_word = 0;
                        if (rram && sp + 3 < mac_mem_get_ram_size()) {
                            ret_addr = ((uint32_t)rram[sp]<<24)|
                                       ((uint32_t)rram[sp+1]<<16)|
                                       ((uint32_t)rram[sp+2]<<8)|
                                       rram[sp+3];
                            uint32_t tw_addr = (ret_addr - 2) & 0xFFFFFF;
                            /* Read trap word from ROM or RAM */
                            trap_word = m68k_read_memory_16(tw_addr);
                        }
                        /* Track histogram of trap words */
                        int found = -1;
                        for (int i = 0; i < 16 && i < safe_cnt; i++) {
                            if (trap_hist[i] == trap_word) { found = i; break; }
                        }
                        if (found >= 0) {
                            trap_hist_cnt[found]++;
                        } else if (safe_cnt < 16) {
                            trap_hist[safe_cnt] = trap_word;
                            trap_hist_cnt[safe_cnt] = 1;
                            found = safe_cnt;
                        }
                        safe_cnt++;
                        /* Log first 30, then periodically */
                        if (safe_cnt <= 30 || safe_cnt % 500 == 0) {
                            ESP_LOGW(TAG, "SAFE #%d trap=$%04X ret=$%06X F%d",
                                     safe_cnt, trap_word, ret_addr & 0xFFFFFF,
                                     frame_count);
                        }
                        /* Dump histogram at key counts */
                        if (safe_cnt == 100 || safe_cnt == 1000 || safe_cnt == 5000) {
                            ESP_LOGW(TAG, "TRAP-HIST (total=%d):", safe_cnt);
                            for (int i = 0; i < 16 && i < safe_cnt; i++) {
                                if (trap_hist_cnt[i] > 0) {
                                    ESP_LOGW(TAG, "  $%04X: %d", trap_hist[i], trap_hist_cnt[i]);
                                }
                            }
                        }
                    }

                    /* .SONY driver entry */
                    if (pcd == 0x434BAC) {
                        ESP_LOGW("sony", "ENTRY 434BAC D0=%08X D1=%04X A1=%06X",
                                 m68k_get_reg(NULL, M68K_REG_D0),
                                 m68k_get_reg(NULL, M68K_REG_D1) & 0xFFFF,
                                 m68k_get_reg(NULL, M68K_REG_A1) & 0xFFFFFF);
                    } else if (pcd == 0x434BC0 && trace_count < 20) {
                        ESP_LOGW("sony", "RDDATA-CHK 434BC0 D0=%08X SR=%04X",
                                 m68k_get_reg(NULL, M68K_REG_D0),
                                 m68k_get_reg(NULL, M68K_REG_SR));
                        trace_count++;
                    } else if (pcd == 0x434BD4) {
                        ESP_LOGW("sony", "PROCEED 434BD4 (RDDATA=0)");
                        trace_count = 0;
                    }
                    /* Sector read loop: $4357D0 entry, $4357D8 check, $435856 error */
                    else if (pcd == 0x4357D0 && secrd_count < 10) {
                        ESP_LOGW("sony", "SEC-RD %06X D3=%ld A1=%06X D1=%04X",
                                 pcd,
                                 (long)m68k_get_reg(NULL, M68K_REG_D3),
                                 m68k_get_reg(NULL, M68K_REG_A1) & 0xFFFFFF,
                                 m68k_get_reg(NULL, M68K_REG_D1) & 0xFFFF);
                        secrd_count++;
                    } else if (pcd == 0x4357D8 && secrd_count <= 10) {
                        /* At this point, A1 and D1 are set by $435122.
                         * The check is TST.B (4,A1,D1.W).  Read that byte. */
                        uint32_t a1 = m68k_get_reg(NULL, M68K_REG_A1);
                        uint16_t d1 = m68k_get_reg(NULL, M68K_REG_D1) & 0xFFFF;
                        uint8_t *ram = mac_mem_get_ram();
                        uint32_t addr = (a1 + d1 + 4) & 0xFFFFFF;
                        uint8_t val = (ram && addr < 0x200000) ? ram[addr] : 0xFF;
                        ESP_LOGW("sony", "SEC-CHK %06X A1=%06X D1=%04X [A1+D1+4]=%06X val=%02X",
                                 pcd, a1 & 0xFFFFFF, d1, addr, val);
                    } else if (pcd == 0x435856 && secrd_count <= 10) {
                        ESP_LOGW("sony", "SEC-EXIT %06X (RTS)", pcd);
                    }
                    /* $4357D0 flow: three-way branch decision */
                    else if (pcd == 0x4357DE) {
                        static int c = 0;
                        if (c++ < 20) {
                            uint32_t a1 = m68k_get_reg(NULL, M68K_REG_A1);
                            uint16_t d1 = m68k_get_reg(NULL, M68K_REG_D1) & 0xFFFF;
                            uint8_t *ram = mac_mem_get_ram();
                            uint32_t addr3 = (a1 + d1 + 3) & 0xFFFFFF;
                            uint32_t addr5 = (a1 + d1 + 5) & 0xFFFFFF;
                            int8_t v3 = (ram && addr3 < 0x200000) ? (int8_t)ram[addr3] : 0;
                            int8_t v5 = (ram && addr5 < 0x200000) ? (int8_t)ram[addr5] : 0;
                            ESP_LOGW("sony", "DRV-STATE +3=%d +5=%d (0:init >0:retry <0:inc)",
                                     v3, v5);
                        }
                    }
                    else if (pcd == 0x43580C) {
                        static int c = 0;
                        if (c++ < 20)
                            ESP_LOGW("sony", ">>> INIT-PATH $43580C (state==0)");
                    }
                    else if (pcd == 0x4357EE) {
                        static int c = 0;
                        if (c++ < 20)
                            ESP_LOGW("sony", ">>> RETRY-PATH $4357EE (state>0)");
                    }
                    else if (pcd == 0x43582C) {
                        static int c = 0;
                        if (c++ < 20)
                            ESP_LOGW("sony", "HSHK-CHK $43582C (BSR $435198)");
                    }
                    else if (pcd == 0x435830) {
                        static int c = 0;
                        if (c++ < 20) {
                            uint16_t sr = m68k_get_reg(NULL, M68K_REG_SR);
                            ESP_LOGW("sony", "HSHK-BMI $435830 SR=%04X N=%d (N=1 -> PostEvt)",
                                     sr, (sr >> 3) & 1);
                        }
                    }
                    else if (pcd == 0x43583C) {
                        static int c = 0;
                        if (c++ < 10)
                            ESP_LOGW("sony", "*** POST-EVT $43583C (ERROR ok!) ***");
                    }
                    else if (pcd == 0x435832) {
                        static int c = 0;
                        if (c++ < 10)
                            ESP_LOGW("sony", "!!! INIT-NOPE $435832 (no ERROR, exit)");
                    }
                    else if (pcd == 0x43584A) {
                        static int c = 0;
                        if (c++ < 10) {
                            uint32_t a1 = m68k_get_reg(NULL, M68K_REG_A1);
                            uint16_t d1 = m68k_get_reg(NULL, M68K_REG_D1) & 0xFFFF;
                            uint8_t *ram = mac_mem_get_ram();
                            uint32_t addr = (a1 + d1 + 3) & 0xFFFFFF;
                            int8_t val = (ram && addr < 0x200000) ? (int8_t)ram[addr] : 0;
                            ESP_LOGW("sony", "STATE-INC $43584A state=%d->%d", val, val+1);
                        }
                    }
                    else if (pcd == 0x43584E) {
                        static int c = 0;
                        if (c++ < 10)
                            ESP_LOGW("sony", "ST-DRIVE $43584E (byte+0 = FF)");
                    }
                    /* ISM init caller detection */
                    else if (pcd == 0x435A1C) {
                        ESP_LOGW("sony", "ISM-INIT-ENTRY %06X SP=%06X",
                                 pcd, m68k_get_reg(NULL, M68K_REG_SP));
                    }
                    /* Post-ISM-init: code at 435A88 */
                    else if (pcd == 0x435A88) {
                        ESP_LOGW("sony", "POST-INIT %06X", pcd);
                    } else if (pcd == 0x435A94) {
                        ESP_LOGW("sony", "POST-INIT-2 %06X (MODE0=$38 path)", pcd);
                    }
                    /* Post-ISM-init sector verification flow */
                    else if (pcd == 0x43477E) {
                        ESP_LOGW("sony", "SECTOR-VERIFY-ENTRY %06X D0=%08X D2=%04X A1=%06X",
                                 pcd, m68k_get_reg(NULL, M68K_REG_D0),
                                 m68k_get_reg(NULL, M68K_REG_D2) & 0xFFFF,
                                 m68k_get_reg(NULL, M68K_REG_A1) & 0xFFFFFF);
                    } else if (pcd == 0x434850) {
                        /* Return from sector verification — log D0 and return address */
                        uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
                        uint8_t *ram = mac_mem_get_ram();
                        uint32_t retaddr = 0;
                        if (ram && sp > 0 && sp + 3 < 0x200000) {
                            retaddr = ((uint32_t)ram[sp]<<24)|((uint32_t)ram[sp+1]<<16)|
                                      ((uint32_t)ram[sp+2]<<8)|ram[sp+3];
                        }
                        ESP_LOGW("sony", "SECTOR-VERIFY-RTS %06X D0=%08X ret=%06X F%d",
                                 pcd, m68k_get_reg(NULL, M68K_REG_D0),
                                 retaddr & 0xFFFFFF, frame_count);
                    } else if (pcd == 0x434840) {
                        static int loop_exit_count = 0;
                        if (loop_exit_count < 20) {
                            ESP_LOGW("sony", "LOOP-EXIT %06X D2=%04X D3=%08X",
                                     pcd,
                                     m68k_get_reg(NULL, M68K_REG_D2) & 0xFFFF,
                                     m68k_get_reg(NULL, M68K_REG_D3));
                            loop_exit_count++;
                        }
                    }
                    /* Disk event handler in main loop */
                    else if (pcd == 0x435782) {
                        static int devt_count = 0;
                        if (devt_count < 5) {
                            ESP_LOGW("sony", "DISK-EVT %06X D3=%08X F%d",
                                     pcd, m68k_get_reg(NULL, M68K_REG_D3), frame_count);
                            devt_count++;
                        }
                    }
                    /* A-line trap detection (trap dispatch area, typically $0F0000+) */
                    else if (pcd == 0x434BA2 || pcd == 0x434BA8) {
                        ESP_LOGW("sony", "SONY-PREAMBLE %06X", pcd);
                    }
                    /* Boot loop _Read detection */
                    else if (pcd == 0x400DF4 && !read_entry_seen) {
                        uint8_t *rram = mac_mem_get_ram();
                        uint32_t a0v = m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF;
                        uint16_t ioref = 0;
                        if (rram && a0v + 0x19 < 0x200000) {
                            ioref = ((uint16_t)rram[a0v + 0x18] << 8) | rram[a0v + 0x19];
                        }
                        ESP_LOGW(TAG, "READ-ENTRY F%d A0=%06X A6=%06X ioRef=%04X D0=%08X",
                                 frame_count, a0v,
                                 m68k_get_reg(NULL, M68K_REG_A6) & 0xFFFFFF,
                                 ioref, m68k_get_reg(NULL, M68K_REG_D0));
                        read_entry_seen = true;
                    }
                    else if (pcd == 0x400E0C) {
                        /* BNE after _Read — D0 = result, Z should be 1 */
                        static int bne_log = 0;
                        if (bne_log < 5) {
                            ESP_LOGW(TAG, "READ-BNE F%d D0=%08X SR=%04X",
                                     frame_count, m68k_get_reg(NULL, M68K_REG_D0),
                                     m68k_get_reg(NULL, M68K_REG_SR));
                            bne_log++;
                        }
                    }
                    else if (pcd == 0x400E0E) {
                        /* CMPI.W #$4C4B,(A6) — check boot block sig */
                        static int sig_log = 0;
                        uint8_t *rram = mac_mem_get_ram();
                        uint32_t a6v = m68k_get_reg(NULL, M68K_REG_A6) & 0xFFFFFF;
                        uint16_t sig = 0;
                        if (rram && a6v + 1 < 0x200000) {
                            sig = ((uint16_t)rram[a6v] << 8) | rram[a6v + 1];
                        }
                        if (sig_log < 5) {
                            ESP_LOGW(TAG, "BOOT-SIG-CHK F%d A6=%06X sig=%04X (want 4C4B)",
                                     frame_count, a6v, sig);
                            sig_log++;
                        }
                    }
                    /* Event loop diagnostic: track where the boot loop is stuck */
                    else if (pcd == 0x400CAC) {
                        /* BEQ $400C9C — no event found, loop back.
                         * Log periodically to see if loop is alive. */
                        static int evt_miss = 0;
                        evt_miss++;
                        if (evt_miss == 1 || (evt_miss % 5000 == 0)) {
                            ESP_LOGW(TAG, "EVT-MISS %d F%d: no disk event in queue",
                                     evt_miss, frame_count);
                        }
                    }
                    else if (pcd == 0x400CAE) {
                        /* BSR $400DCA — event found! */
                        ESP_LOGW(TAG, "EVT-FOUND F%d: disk event processed", frame_count);
                    }
                    else if (pcd == 0x400CB2) {
                        /* BSR $400DE8 — gate check reached */
                        uint8_t *rram = mac_mem_get_ram();
                        uint8_t v172 = (rram) ? rram[0x172] : 0xFF;
                        ESP_LOGW(TAG, "GATE-CHK F%d: $172=%02X", frame_count, v172);
                    }
                    else if (pcd == 0x400CC4) {
                        ESP_LOGW(TAG, "*** BOOT-BRA F%d: BRA $400E72 ***",
                                 frame_count);
                    }
                    else if (pcd == 0x400E72) {
                        ESP_LOGW(TAG, "*** BOOT EXIT F%d: A852 trap ***",
                                 frame_count);
                    }
                    else if (pcd == 0x400E74) {
                        ESP_LOGW(TAG, "BOOT-CONT F%d: MOVEM (past A852)",
                                 frame_count);
                    }
                    else if (pcd == 0x401176) {
                        static bool s_logged = false;
                        if (!s_logged) {
                            ESP_LOGW(TAG, "BOOT-JMP F%d: $401176 entry SP=%06X",
                                     frame_count,
                                     m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
                            s_logged = true;
                        }
                    }
                    else if (pcd == 0x400E80) {
                        static bool s_logged = false;
                        if (!s_logged) {
                            ESP_LOGW(TAG, "*** INIT DONE $400E80 F%d ***", frame_count);
                            s_logged = true;
                        }
                    }
#if MAC_ROM_LC_II
                    else if (pcd == 0x600EBE || pcd == 0x600352) {
                        static int v8dec = 0;
                        if (v8dec < 12) {
                            ESP_LOGW(TAG, "V8DEC #%d: PC=%06X A0=%06X A1=%06X A3=%06X D6=%08X",
                                     v8dec++, pcd,
                                     m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF,
                                     m68k_get_reg(NULL, M68K_REG_A1) & 0xFFFFFF,
                                     m68k_get_reg(NULL, M68K_REG_A3) & 0xFFFFFF,
                                     m68k_get_reg(NULL, M68K_REG_D6));
                        }
                    }
#endif
#if MAC_ROM_LC_II
                    else if (pcd >= 0x5FFFD2 && pcd < 0x610000) {
                        static int sm_log = 0;
                        static uint32_t last_sm_pc = 0xFFFFFFFF;
                        if (sm_log < 60 && pcd != last_sm_pc) {
                            last_sm_pc = pcd;
                            ESP_LOGW(TAG, "SM #%d: PC=%06X F%d", sm_log++, pcd, frame_count);
                        }
                    }
#endif
                    else if (pcd >= 0x100000 && pcd < 0x200000) {
                        static int bb_log = 0;
                        static uint32_t last_bb_pc = 0;
                        if (bb_log < 40) {
                            uint8_t *rr = mac_mem_get_ram();
                            uint32_t rsz = mac_mem_get_ram_size();
                            uint32_t ro = pcd & (rsz - 1);
                            ESP_LOGW(TAG, "BB PC=%06X SP=%06X F%d B=%02X%02X%02X%02X",
                                     pcd, m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF,
                                     frame_count,
                                     rr ? rr[ro] : 0, rr ? rr[ro+1] : 0,
                                     rr ? rr[ro+2] : 0, rr ? rr[ro+3] : 0);
                            bb_log++;
                        }
                        last_bb_pc = pcd;
                    }
                    /* Trace $404490 (the RTS that pops garbage in the F631 crash).
                     * Log the stack top BEFORE the RTS executes. */
                    else if (pcd == 0x404490 && frame_count >= 600 && frame_count <= 700) {
                        static int c_rts = 0;
                        if (c_rts < 10) {
                            uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF;
                            uint8_t *rram = mac_mem_get_ram();
                            uint32_t rsz = mac_mem_get_ram_size();
                            if (rram && sp < rsz && sp + 15 < rsz) {
                                uint32_t s0 = ((uint32_t)rram[sp]<<24)|((uint32_t)rram[sp+1]<<16)|
                                              ((uint32_t)rram[sp+2]<<8)|rram[sp+3];
                                uint32_t s1 = ((uint32_t)rram[sp+4]<<24)|((uint32_t)rram[sp+5]<<16)|
                                              ((uint32_t)rram[sp+6]<<8)|rram[sp+7];
                                uint32_t s2 = ((uint32_t)rram[sp+8]<<24)|((uint32_t)rram[sp+9]<<16)|
                                              ((uint32_t)rram[sp+10]<<8)|rram[sp+11];
                                uint32_t ret24 = s0 & 0xFFFFFF;
                                const char *tag = "";
                                if ((ret24 >= 0x200000 && ret24 < 0x400000) ||
                                    ret24 >= 0x480000) {
                                    tag = " *** BAD ***";
                                }
                                ESP_LOGW(TAG, "PRE-RTS $404490 SP=$%06X [%08lX %08lX %08lX] F%d%s",
                                         sp, (unsigned long)s0, (unsigned long)s1,
                                         (unsigned long)s2, frame_count, tag);
                            }
                            c_rts++;
                        }
                    }
                    /* Detect invalid PC: outside RAM ($0-$1FFFFF) and ROM ($400000-$47FFFF). */
                    else if ((pcd >= 0x200000 && pcd < 0x400000) ||
                             (pcd >= 0x480000)) {
                        static int inv_log = 0;
                        if (inv_log == 0) {
                            uint8_t *rram = mac_mem_get_ram();
                            uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF;
                            ESP_LOGE(TAG, "!FIRST-BAD-PC $%06X SP=$%06X SR=%04X D0=%08X F%d",
                                     pcd, sp, m68k_get_reg(NULL, M68K_REG_SR),
                                     m68k_get_reg(NULL, M68K_REG_D0), frame_count);
                            ESP_LOGE(TAG, "  prev PCs: $%06X $%06X $%06X $%06X",
                                     bad_pc_hist[(bad_pc_idx+0)%4],
                                     bad_pc_hist[(bad_pc_idx+1)%4],
                                     bad_pc_hist[(bad_pc_idx+2)%4],
                                     bad_pc_hist[(bad_pc_idx+3)%4]);
                            if (rram && sp < mac_mem_get_ram_size() - 32) {
                                uint32_t sm = sp & (mac_mem_get_ram_size()-1);
                                ESP_LOGE(TAG, "  stack: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                                    rram[sm],rram[sm+1],rram[sm+2],rram[sm+3],
                                    rram[sm+4],rram[sm+5],rram[sm+6],rram[sm+7],
                                    rram[sm+8],rram[sm+9],rram[sm+10],rram[sm+11],
                                    rram[sm+12],rram[sm+13],rram[sm+14],rram[sm+15]);
                            }
                            ESP_LOGE(TAG, "  A0=$%06X A1=$%06X A2=$%06X A3=$%06X",
                                     m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF,
                                     m68k_get_reg(NULL, M68K_REG_A1) & 0xFFFFFF,
                                     m68k_get_reg(NULL, M68K_REG_A2) & 0xFFFFFF,
                                     m68k_get_reg(NULL, M68K_REG_A3) & 0xFFFFFF);
                            ESP_LOGE(TAG, "  A4=$%06X A5=$%06X A6=$%06X",
                                     m68k_get_reg(NULL, M68K_REG_A4) & 0xFFFFFF,
                                     m68k_get_reg(NULL, M68K_REG_A5) & 0xFFFFFF,
                                     m68k_get_reg(NULL, M68K_REG_A6) & 0xFFFFFF);
                            ESP_LOGE(TAG, "  D1=$%08X D2=$%08X D3=$%08X D4=$%08X",
                                     m68k_get_reg(NULL, M68K_REG_D1),
                                     m68k_get_reg(NULL, M68K_REG_D2),
                                     m68k_get_reg(NULL, M68K_REG_D3),
                                     m68k_get_reg(NULL, M68K_REG_D4));

                            /* === TRAP RING DUMP ===
                             * Show the last 32 A-line traps before the crash. */
                            {
                                int n = trap_ring_idx < 32 ? trap_ring_idx : 32;
                                ESP_LOGW(TAG, "Last %d traps (of %d total):", n, trap_ring_idx);
                                for (int i = 0; i < n; i++) {
                                    int idx = ((trap_ring_idx - n + i) % 32 + 32) % 32;
                                    ESP_LOGW(TAG, "  T[%d] $%04X from $%06X",
                                             i, trap_ring[idx],
                                             trap_caller_ring[idx] & 0xFFFFFF);
                                }
                            }

                            /* === CRASH RECOVERY ===
                             * Prevent cascade: redirect to tight loop, reset SP.
                             * Don't touch MemTop/ScrnBase — use ROM's values. */
                            if (!crash_recovered && rram && !MAC_ROM_LC_II) {
                                crash_recovered = true;
                                m68k_set_reg(M68K_REG_SP, 0x0FFA00);
                                m68k_set_reg(M68K_REG_PC, 0x000036);
                                ESP_LOGW(TAG, "RECOVERY: PC=$0036 SP=$0FFA00");
                            }
                        }
                        inv_log++;
                    }
                    else if (pcd == 0x434BAC) {
                        ESP_LOGW(TAG, "*** SONY DRIVER ENTRY *** F%d", frame_count);
                    }
                    /* ISM sector-read entry and its callers — why is $17D never set? */
                    else if (pcd == 0x435A88) {
                        static int c0 = 0;
                        if (c0++ < 10) ESP_LOGW(TAG, "*** ISM-SEC-ENTRY $435A88 F%d ***", frame_count);
                    }
                    else if (pcd == 0x434BA8) {
                        static int c1 = 0;
                        if (c1++ < 10) {
                            uint8_t *ram = mac_mem_get_ram();
                            uint32_t a1v = m68k_get_reg(NULL, M68K_REG_A1) & 0xFFFFFF;
                            uint8_t v17c = (ram && a1v + 0x17C < 0x200000) ? ram[a1v + 0x17C] : 0xFF;
                            ESP_LOGW(TAG, "CALLER-434BA8 F%d A1=%06X $17C=%02X", frame_count, a1v, v17c);
                        }
                    }
                    else if (pcd == 0x434BE6) {
                        static int c2 = 0;
                        if (c2++ < 10) ESP_LOGW(TAG, "CALLER-434BE6 F%d", frame_count);
                    }
                    else if (pcd == 0x435410) {
                        static int c3 = 0;
                        if (c3++ < 10) ESP_LOGW(TAG, "CALLER-435410 F%d", frame_count);
                    }
                    else if (pcd == 0x436A7E) {
                        static int c4 = 0;
                        if (c4++ < 10) ESP_LOGW(TAG, "CALLER-436A7E F%d", frame_count);
                    }
                    else if (pcd == 0x436B18) {
                        static int c5 = 0;
                        if (c5++ < 10) ESP_LOGW(TAG, "CALLER-436B18 F%d", frame_count);
                    }
#if !MAC_ROM_LC_II
                    /* SEC-RD drive ≥ 4 bypass: at $4357F4, BGE $435856
                     * skips the sector-read sub for drives with DV+$12 ≥ 4.
                     * Our drive is #5 (DV+$12=5).  Force BGE to fall through
                     * so BSR $435538 (sector-read sub) executes.  The ISM
                     * probe inside $435538 passes thanks to the $13E patch.
                     * Method: set SR N=1, V=0 so BGE condition (N⊕V=0) fails. */
                    else if (pcd == 0x4357F4) {
                        uint16_t sr = m68k_get_reg(NULL, M68K_REG_SR);
                        sr |= 0x0008;   /* Set N */
                        sr &= ~0x0002;  /* Clear V */
                        m68k_set_reg(M68K_REG_SR, sr);
                    }
#endif
                    /* Also trace $435196 and $435538 — the ISM/IWM branch points */
                    else if (pcd == 0x435196) {
                        static int c6 = 0;
                        if (c6++ < 10) {
                            uint8_t *ram = mac_mem_get_ram();
                            uint32_t a1v = m68k_get_reg(NULL, M68K_REG_A1) & 0xFFFFFF;
                            uint8_t v17d = (ram && a1v + 0x17D < 0x200000) ? ram[a1v + 0x17D] : 0xFF;
                            ESP_LOGW(TAG, "BRANCH-435196 F%d A1=%06X $17D=%02X", frame_count, a1v, v17d);
                        }
                    }
                    else if (pcd == 0x435538) {
                        static int c7 = 0;
                        if (c7++ < 10) {
                            uint8_t *ram = mac_mem_get_ram();
                            uint32_t a1v = m68k_get_reg(NULL, M68K_REG_A1) & 0xFFFFFF;
                            uint8_t v17d = (ram && a1v + 0x17D < 0x200000) ? ram[a1v + 0x17D] : 0xFF;
                            ESP_LOGW(TAG, "BRANCH-435538 F%d A1=%06X $17D=%02X", frame_count, a1v, v17d);
                        }
                    }
                    last_trace_pc = pcd;
                }
            }

            /* Post-F700 resource trap monitor: lightweight Line-A intercept.
             * Catches _GetResource etc. during System file resource loading
             * phase which extends well beyond F700.  Only 2 comparisons
             * per slice — negligible overhead with normal_slice=1000. */
            if (mac_boot_read_bypass && frame_count > 700 && frame_count <= 2000) {
                if (pc_slice == line_a_handler_addr && line_a_handler_addr != 0) {
                    uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF;
                    uint8_t *rram = mac_mem_get_ram();
                    uint32_t rsz = mac_mem_get_ram_size();
                    if (rram && sp + 13 < rsz) {
                        uint32_t saved_pc = ((uint32_t)rram[sp+2]<<24)|
                                            ((uint32_t)rram[sp+3]<<16)|
                                            ((uint32_t)rram[sp+4]<<8)|rram[sp+5];
                        uint32_t spc = saved_pc & 0xFFFFFF;
                        uint16_t tw = m68k_read_memory_16(spc);
                        /* Update trap ring */
                        int ri = trap_ring_idx % 32;
                        trap_ring[ri] = tw;
                        trap_caller_ring[ri] = spc;
                        trap_ring_idx++;
                        /* Log resource manager traps */
                        if (tw == 0xA9A0 || tw == 0xA9A7 ||
                            tw == 0xA99C || tw == 0xA80D ||
                            tw == 0xA997 || tw == 0xA99F) {
                            static int res2_cnt = 0;
                            if (res2_cnt < 200) {
                                int16_t res_id = (int16_t)((rram[sp+6]<<8)|rram[sp+7]);
                                char rtype[5];
                                rtype[0] = rram[sp+8]; rtype[1] = rram[sp+9];
                                rtype[2] = rram[sp+10]; rtype[3] = rram[sp+11];
                                rtype[4] = '\0';
                                for (int ci = 0; ci < 4; ci++)
                                    if (rtype[ci] < 0x20 || rtype[ci] > 0x7E) rtype[ci] = '?';
                                const char *tname =
                                    (tw == 0xA9A0) ? "GetRes" :
                                    (tw == 0xA9A7) ? "Get1Res" :
                                    (tw == 0xA99C) ? "GetIndRes" :
                                    (tw == 0xA80D) ? "Cnt1Res" :
                                    (tw == 0xA997) ? "OpenRF" : "ReleaseRes";
                                ESP_LOGW(TAG, "RES2 #%d: %s('%s',%d) PC=$%06X F%d",
                                         res2_cnt, tname, rtype, res_id,
                                         spc, frame_count);
                            }
                            res2_cnt++;
                        }
                    }
                }
            }

            /* Post-boot safe handler detection (runs ALWAYS, not just F<=700).
             * Lightweight: one PC read + comparison per slice. */
            if (mac_boot_read_bypass && frame_count > 700) {
                if (pc_slice == 0x000030) {
                    static int post_safe_cnt = 0;
                    post_safe_cnt++;
                    if (post_safe_cnt <= 50 || post_safe_cnt % 200 == 0) {
                        uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF;
                        uint8_t *rram = mac_mem_get_ram();
                        uint32_t ret_addr = 0;
                        uint16_t trap_word = 0;
                        if (rram && sp + 3 < mac_mem_get_ram_size()) {
                            ret_addr = ((uint32_t)rram[sp]<<24)|
                                       ((uint32_t)rram[sp+1]<<16)|
                                       ((uint32_t)rram[sp+2]<<8)|rram[sp+3];
                            uint32_t tw_addr = (ret_addr - 2) & 0xFFFFFF;
                            trap_word = m68k_read_memory_16(tw_addr);
                        }
                        ESP_LOGW(TAG, "POST-SAFE #%d trap=$%04X ret=$%06X F%d",
                                 post_safe_cnt, trap_word,
                                 ret_addr & 0xFFFFFF, frame_count);
                    }
                }
            }

            /* One-shot trap table dump at F2000 — show which OS traps
             * are still pointing to our safe handler ($0030) vs System
             * implementations vs ROM handlers.
             * Also enables post-boot optimizations (lean instruction fetch). */
            {
            static bool trap_table_dumped = false;
            if (frame_count >= 2000 && !trap_table_dumped) {
                trap_table_dumped = true;
                /* Enable lean instruction fetch path for M68K_SEPARATE_READS */
                if (!mac_post_boot) {
                    mac_post_boot = true;
                    ESP_LOGI(TAG, "POST-BOOT: lean instruction fetch enabled at F%d", frame_count);
                }
                uint8_t *rram = mac_mem_get_ram();
                if (rram) {
                    int cnt_safe = 0, cnt_rom = 0, cnt_sys = 0, cnt_zero = 0;
                    for (int i = 0; i < 256; i++) {
                        uint32_t addr = 0x0400 + i * 4;
                        uint32_t entry = ((uint32_t)rram[addr]<<24)|
                                         ((uint32_t)rram[addr+1]<<16)|
                                         ((uint32_t)rram[addr+2]<<8)|
                                         rram[addr+3];
                        if (entry == 0x000030) cnt_safe++;
                        else if (entry == 0) cnt_zero++;
                        else if (entry >= 0x400000 && entry < 0x480000) cnt_rom++;
                        else cnt_sys++;
                    }
                    ESP_LOGW(TAG, "TRAP-TABLE F2000: safe=%d rom=%d sys=%d zero=%d",
                             cnt_safe, cnt_rom, cnt_sys, cnt_zero);
                    /* Dump critical File Manager & Resource Manager OS traps */
                    static const struct { uint8_t idx; const char *name; } fm_traps[] = {
                        {0x00,"_Open"}, {0x01,"_Close"}, {0x02,"_Read"},
                        {0x03,"_Write"}, {0x07,"_GetVolInfo"}, {0x08,"_Create"},
                        {0x09,"_Delete"}, {0x0A,"_OpenRF"}, {0x0D,"_GetFileInfo"},
                        {0x15,"_MountVol"}, {0x18,"_SetFPos"}, {0x60,"_FSDispatch"},
                    };
                    for (int t = 0; t < (int)(sizeof(fm_traps)/sizeof(fm_traps[0])); t++) {
                        uint32_t a = 0x0400 + fm_traps[t].idx * 4;
                        uint32_t v = ((uint32_t)rram[a]<<24)|((uint32_t)rram[a+1]<<16)|
                                     ((uint32_t)rram[a+2]<<8)|rram[a+3];
                        const char *tag = (v == 0x000030) ? " SAFE" :
                                          (v >= 0x400000 && v < 0x480000) ? " ROM" : " SYS";
                        ESP_LOGW(TAG, "  OS[$%02X] %s=$%06lX%s",
                                 fm_traps[t].idx, fm_traps[t].name,
                                 (unsigned long)(v & 0xFFFFFF), tag);
                    }
                    /* Check critical Toolbox traps (QuickDraw, Window Mgr, etc.) */
                    {
                        static const struct { uint16_t idx; const char *name; } tb_traps[] = {
                            {0x06E,"_InitGraf"}, {0x0FE,"_InitFonts"},
                            {0x112,"_InitWindows"}, {0x130,"_InitMenus"},
                            {0x091,"_TEInit"}, {0x17B,"_InitDialogs"},
                            {0x055,"_DrawMenuBar"}, {0x1F2,"_Launch"},
                            {0x052,"_HideCursor"}, {0x053,"_ShowCursor"},
                            {0x0A7,"_Get1Resource"}, {0x0AF,"_ResError"},
                        };
                        uint32_t unimp = 0x400768;
                        int tb_unimp = 0;
                        for (int t = 0; t < (int)(sizeof(tb_traps)/sizeof(tb_traps[0])); t++) {
                            uint32_t a = 0x0E00 + tb_traps[t].idx * 4;
                            uint32_t v = ((uint32_t)rram[a]<<24)|((uint32_t)rram[a+1]<<16)|
                                         ((uint32_t)rram[a+2]<<8)|rram[a+3];
                            const char *tag = (v == unimp) ? " ***UNIMP***" :
                                              (v == 0) ? " ***ZERO***" :
                                              (v >= 0x400000 && v < 0x480000) ? " ROM" : " SYS";
                            if (v == unimp || v == 0) tb_unimp++;
                            ESP_LOGW(TAG, "  TB[$%03X] %s=$%06lX%s",
                                     tb_traps[t].idx, tb_traps[t].name,
                                     (unsigned long)(v & 0xFFFFFF), tag);
                        }
                        if (tb_unimp > 0) {
                            ESP_LOGE(TAG, "*** %d critical Toolbox traps are UNIMPLEMENTED! ***",
                                     tb_unimp);
                        }
                        /* Check key low-mem globals for Finder launch state */
                        uint32_t wmgr_port = ((uint32_t)rram[0x9DE]<<24)|
                                             ((uint32_t)rram[0x9DF]<<16)|
                                             ((uint32_t)rram[0x9E0]<<8)|rram[0x9E1];
                        uint32_t gray_rgn = ((uint32_t)rram[0xA1C]<<24)|
                                            ((uint32_t)rram[0xA1D]<<16)|
                                            ((uint32_t)rram[0xA1E]<<8)|rram[0xA1F];
                        int fnl = rram[0x2E0];
                        char fname[16] = {0};
                        if (fnl > 0 && fnl < 16)
                            for (int i = 0; i < fnl; i++) fname[i] = rram[0x2E1+i];
                        int anl = rram[0x910];
                        char appname[32] = {0};
                        if (anl > 0 && anl < 32)
                            for (int i = 0; i < anl; i++) appname[i] = rram[0x911+i];
                        uint16_t boot_drv = (rram[0x210]<<8)|rram[0x211];
                        int16_t sys_map = (int16_t)((rram[0xA58]<<8)|rram[0xA59]);
                        ESP_LOGW(TAG, "  WMgrPort=$%08lX GrayRgn=$%08lX",
                                 (unsigned long)wmgr_port, (unsigned long)gray_rgn);
                        ESP_LOGW(TAG, "  FinderName=\"%s\" CurApName=\"%s\"",
                                 fname, appname);
                        ESP_LOGW(TAG, "  BootDrive=%d SysMap=%d", boot_drv, sys_map);
                    }
                }
            }
            }

            /* Advance SWIM byte clock — simulates MFM byte arrival timing.
             * Must be called BEFORE via_tick so RDDATA is updated before
             * the next ISM register access. */
            iwm_tick(executed);

            /* Update VIA timers — run at normal speed even during disk reads.
             * The ROM's .SONY driver uses VIA-based timeouts to detect
             * when a full track has been read.  Let the timeout fire
             * naturally so the driver exits its byte-reading loop. */
            via_tick(executed * MAC_CYCLE_SCALE);   /* device cycles */
#if MAC_SOUND
            asc_tick(executed * MAC_CYCLE_SCALE);
#endif

            /* Check for VIA interrupt -> raise CPU IRQ level 1 */
#if MAC_ROM_LC_II
            via_update_cpu_irq();       /* also level 2 (pseudo-VIA2) */
#else
            if (via_irq_pending()) {
                m68k_set_irq(M68K_IRQ_1);
            } else {
                m68k_set_irq(M68K_IRQ_NONE);
            }
#endif

#if MAC_IDLE_FF
            /* Idle fast-forward (see ff_* above). Device time advances in
             * fast_slice chunks exactly as during a spin; the CPU resumes at
             * the same PC once an interrupt it will take is pending. */
            {
                bool ff = ff_request;
                ff_request = false;
                if (!ff && mac_post_boot && cycles_remaining > 0 &&
                    ff_ticks_wait_at(pc_slice)) {
                    ff = true;
                    ff_stat_tick++;
                }
                if (ff && cycles_remaining > 0 && mac_post_boot && !iwm_is_reading()) {
                    int mask = (m68k_get_reg(NULL, M68K_REG_SR) >> 8) & 7;
                    bool woke = false;
                    ff_stat_count++;
                    while (cycles_remaining > 0) {
                        if (ff_pending_level() > mask) { woke = true; break; }
                        int c = (cycles_remaining > fast_slice) ? fast_slice : cycles_remaining;
                        cycles_remaining -= c;
                        total_executed += c;
                        ff_stat_skipped += c;
                        via_tick(c * MAC_CYCLE_SCALE);
#if MAC_SOUND
                        asc_tick(c * MAC_CYCLE_SCALE);
#endif
                        if (!vblank_fired && total_executed >= MAC_CPU_CYCLES_PER_FRAME / 2) {
                            via_vblank();
#if MAC_ROM_LC_II
                            v8video_vbl(true);
#endif
                            vblank_fired = true;
                        }
#if MAC_ROM_LC_II
                        via_update_cpu_irq();
#else
                        m68k_set_irq(via_irq_pending() ? M68K_IRQ_1 : M68K_IRQ_NONE);
#endif
                    }
                    if (woke || ff_pending_level() > mask) {
                        ff_resume_armed = true;
                        ff_park_pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
                    }
                }
            }
#endif
        }

        /* Boot-only per-frame checks (single PC read shared between them) */
        if (frame_count < 2000) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            /* Dispatch loop detection */
            if (pc >= 0x401F38 && pc <= 0x402500) {
                dispatch_frames++;
                if (dispatch_frames == 5) {
                    ESP_LOGW(TAG, "DISPATCH LOOP still entered at F%d PC=$%06X SP=$%06X",
                             frame_count, pc,
                             m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
                }
            } else {
                dispatch_frames = 0;
            }
            /* IWM busy-wait bypass */
            if (pc >= 0x40335E && pc <= 0x403364) {
                iwm_poll_frames++;
                if (iwm_poll_frames > 3) {
                    uint32_t a3 = m68k_get_reg(NULL, M68K_REG_A3) & 0xFFFFFF;
                    uint32_t flag_addr = a3 + 0x015D;
                    uint8_t *ram_ptr = mac_mem_get_ram();
                    if (ram_ptr && flag_addr < mac_mem_get_ram_size()) {
                        ram_ptr[flag_addr] &= ~(1 << 5);
                        iwm_bypass_count++;
                        iwm_poll_frames = 0;
                    }
                }
            } else {
                iwm_poll_frames = 0;
            }
        }

        /* === ALL PATCHES AT F600 ===
         * Install bypass flag, exception handlers, trap table patches,
         * safe handler, and I/O queue all at once. The ROM's initialization
         * is complete and all trap tables are fully populated by now.
         * Disk insertion follows at F650 so the entire boot sequence runs
         * with all patches in place. */
        if (frame_count == 100) {
            uint8_t *rp = mac_mem_get_ram();
            if (rp) {
                uint32_t mt = ((uint32_t)rp[0x108]<<24)|((uint32_t)rp[0x109]<<16)|
                              ((uint32_t)rp[0x10A]<<8)|rp[0x10B];
                uint32_t sb = ((uint32_t)rp[0x824]<<24)|((uint32_t)rp[0x825]<<16)|
                              ((uint32_t)rp[0x826]<<8)|rp[0x827];
                uint32_t sk = ((uint32_t)rp[0x908]<<24)|((uint32_t)rp[0x909]<<16)|
                              ((uint32_t)rp[0x90A]<<8)|rp[0x90B];
                ESP_LOGW(TAG, "F100: MemTop=$%08lX ScrnBase=$%08lX StkBase=$%08lX",
                         (unsigned long)mt, (unsigned long)sb, (unsigned long)sk);
                ESP_LOGW(TAG, "F100CONT $0FFAB0-$0FFAC0:");
                for (int ci = 0; ci < 16; ci += 16) {
                    uint32_t ca = 0x0FFAB0 + ci;
                    ESP_LOGW(TAG, "  $%06X %08X %08X %08X %08X", ca,
                             (rp[ca]<<24)|(rp[ca+1]<<16)|(rp[ca+2]<<8)|rp[ca+3],
                             (rp[ca+4]<<24)|(rp[ca+5]<<16)|(rp[ca+6]<<8)|rp[ca+7],
                             (rp[ca+8]<<24)|(rp[ca+9]<<16)|(rp[ca+10]<<8)|rp[ca+11],
                             (rp[ca+12]<<24)|(rp[ca+13]<<16)|(rp[ca+14]<<8)|rp[ca+15]);
                }
                ESP_LOGW(TAG, "F100CONT $100000-$100040:");
                for (int ci = 0; ci < 64; ci += 16) {
                    uint32_t ca = 0x100000 + ci;
                    ESP_LOGW(TAG, "  $%06X %08X %08X %08X %08X", ca,
                             (rp[ca]<<24)|(rp[ca+1]<<16)|(rp[ca+2]<<8)|rp[ca+3],
                             (rp[ca+4]<<24)|(rp[ca+5]<<16)|(rp[ca+6]<<8)|rp[ca+7],
                             (rp[ca+8]<<24)|(rp[ca+9]<<16)|(rp[ca+10]<<8)|rp[ca+11],
                             (rp[ca+12]<<24)|(rp[ca+13]<<16)|(rp[ca+14]<<8)|rp[ca+15]);
                }
                ESP_LOGW(TAG, "F100CONT $100280-$100290:");
                for (int ci = 0; ci < 16; ci += 16) {
                    uint32_t ca = 0x100280 + ci;
                    ESP_LOGW(TAG, "  $%06X %08X %08X %08X %08X", ca,
                             (rp[ca]<<24)|(rp[ca+1]<<16)|(rp[ca+2]<<8)|rp[ca+3],
                             (rp[ca+4]<<24)|(rp[ca+5]<<16)|(rp[ca+6]<<8)|rp[ca+7],
                             (rp[ca+8]<<24)|(rp[ca+9]<<16)|(rp[ca+10]<<8)|rp[ca+11],
                             (rp[ca+12]<<24)|(rp[ca+13]<<16)|(rp[ca+14]<<8)|rp[ca+15]);
                }
            }
        }
        if (frame_count == 600 && !MAC_ROM_LC_II) {  /* Classic-only boot patches (LC II: vectors/$2000 stub/DrvQ hacks break the V8 boot) */
            mac_boot_read_bypass = true;
            ESP_LOGW(TAG, "BYPASS: All patches at F%d", frame_count);

            uint8_t *ram_ptr = mac_mem_get_ram();
            if (ram_ptr) {
                /* === Dump key low-memory globals BEFORE patching ===
                 * This shows what the ROM initialization set up. */
                {
                    uint32_t mt = ((uint32_t)ram_ptr[0x108]<<24)|((uint32_t)ram_ptr[0x109]<<16)|
                                  ((uint32_t)ram_ptr[0x10A]<<8)|ram_ptr[0x10B];
                    uint32_t bp = ((uint32_t)ram_ptr[0x10C]<<24)|((uint32_t)ram_ptr[0x10D]<<16)|
                                  ((uint32_t)ram_ptr[0x10E]<<8)|ram_ptr[0x10F];
                    uint32_t sb = ((uint32_t)ram_ptr[0x824]<<24)|((uint32_t)ram_ptr[0x825]<<16)|
                                  ((uint32_t)ram_ptr[0x826]<<8)|ram_ptr[0x827];
                    uint32_t sk = ((uint32_t)ram_ptr[0x908]<<24)|((uint32_t)ram_ptr[0x909]<<16)|
                                  ((uint32_t)ram_ptr[0x90A]<<8)|ram_ptr[0x90B];
                    uint32_t al = ((uint32_t)ram_ptr[0x114]<<24)|((uint32_t)ram_ptr[0x115]<<16)|
                                  ((uint32_t)ram_ptr[0x116]<<8)|ram_ptr[0x117];
                    ESP_LOGW(TAG, "PRE-PATCH: MemTop=$%08lX BufPtr=$%08lX ScrnBase=$%08lX StkBase=$%08lX ApplLim=$%08lX",
                             (unsigned long)mt, (unsigned long)bp, (unsigned long)sb,
                             (unsigned long)sk, (unsigned long)al);
                    /* Also dump $2A6 (ROM85) and $28E (ROM version) */
                    uint16_t rom85 = ((uint16_t)ram_ptr[0x2A6]<<8)|ram_ptr[0x2A7];
                    uint16_t romv  = ((uint16_t)ram_ptr[0x28E]<<8)|ram_ptr[0x28F];
                    ESP_LOGW(TAG, "ROM85=$%04X ROMVer=$%04X", rom85, romv);
                }
                /* Dump critical system globals at $0800+ (NOT patched —
                 * these are system parameters, not jump vectors). */
                {
                    uint32_t wmgr = ((uint32_t)ram_ptr[0x9DE]<<24)|((uint32_t)ram_ptr[0x9DF]<<16)|
                                    ((uint32_t)ram_ptr[0x9E0]<<8)|ram_ptr[0x9E1];
                    uint32_t gray = ((uint32_t)ram_ptr[0xA1C]<<24)|((uint32_t)ram_ptr[0xA1D]<<16)|
                                    ((uint32_t)ram_ptr[0xA1E]<<8)|ram_ptr[0xA1F];
                    uint32_t dskp = ((uint32_t)ram_ptr[0x9E2]<<24)|((uint32_t)ram_ptr[0x9E3]<<16)|
                                    ((uint32_t)ram_ptr[0x9E4]<<8)|ram_ptr[0x9E5];
                    /* TB[$112] = _InitWindows handler address */
                    uint32_t iw_addr = 0x0E00 + 0x112 * 4;
                    uint32_t iw = ((uint32_t)ram_ptr[iw_addr]<<24)|((uint32_t)ram_ptr[iw_addr+1]<<16)|
                                  ((uint32_t)ram_ptr[iw_addr+2]<<8)|ram_ptr[iw_addr+3];
                    /* TB[$1F2] = _Launch handler address */
                    uint32_t la_addr = 0x0E00 + 0x1F2 * 4;
                    uint32_t la = ((uint32_t)ram_ptr[la_addr]<<24)|((uint32_t)ram_ptr[la_addr+1]<<16)|
                                  ((uint32_t)ram_ptr[la_addr+2]<<8)|ram_ptr[la_addr+3];
                    ESP_LOGW(TAG, "PRE: WMgrPort=$%08lX GrayRgn=$%08lX DeskPort=$%08lX",
                             (unsigned long)wmgr, (unsigned long)gray, (unsigned long)dskp);
                    ESP_LOGW(TAG, "PRE: _InitWindows=$%08lX _Launch=$%08lX",
                             (unsigned long)iw, (unsigned long)la);
                }

                /* MemTop/ScrnBase: DO NOT force-set — let the ROM use its
                 * own sizing results.  The video task reads ScrnBase from $0824
                 * dynamically, so it adapts to whatever the ROM determined. */

                /* Scan Toolbox table at $0E00 for unimplemented handler */
                uint32_t best_val = 0;
                int best_count = 0;
                for (int i = 0; i < 512; i++) {
                    uint32_t addr = 0x0E00 + i * 4;
                    uint32_t entry = ((uint32_t)ram_ptr[addr]<<24)|
                                     ((uint32_t)ram_ptr[addr+1]<<16)|
                                     ((uint32_t)ram_ptr[addr+2]<<8)|
                                     ram_ptr[addr+3];
                    if (entry == 0) continue;
                    int count = 0;
                    for (int j = 0; j < 512; j++) {
                        uint32_t a2 = 0x0E00 + j * 4;
                        uint32_t e2 = ((uint32_t)ram_ptr[a2]<<24)|
                                      ((uint32_t)ram_ptr[a2+1]<<16)|
                                      ((uint32_t)ram_ptr[a2+2]<<8)|
                                      ram_ptr[a2+3];
                        if (e2 == entry) count++;
                    }
                    if (count > best_count) { best_count = count; best_val = entry; }
                }
                ESP_LOGW(TAG, "TB table: most common=$%08lX (count=%d/512)",
                         (unsigned long)best_val, best_count);

                /* Check TB[$01F] — boot 2 uses $A81F for _Get1Resource */
                {
                    uint32_t tb01f_addr = 0x0E00 + 0x01F * 4;
                    uint32_t tb01f = ((uint32_t)ram_ptr[tb01f_addr]<<24)|
                                     ((uint32_t)ram_ptr[tb01f_addr+1]<<16)|
                                     ((uint32_t)ram_ptr[tb01f_addr+2]<<8)|
                                     ram_ptr[tb01f_addr+3];
                    uint32_t tb1a7_addr = 0x0E00 + 0x1A7 * 4;
                    uint32_t tb1a7 = ((uint32_t)ram_ptr[tb1a7_addr]<<24)|
                                     ((uint32_t)ram_ptr[tb1a7_addr+1]<<16)|
                                     ((uint32_t)ram_ptr[tb1a7_addr+2]<<8)|
                                     ram_ptr[tb1a7_addr+3];
                    uint32_t tb1a0_addr = 0x0E00 + 0x1A0 * 4;
                    uint32_t tb1a0 = ((uint32_t)ram_ptr[tb1a0_addr]<<24)|
                                     ((uint32_t)ram_ptr[tb1a0_addr+1]<<16)|
                                     ((uint32_t)ram_ptr[tb1a0_addr+2]<<8)|
                                     ram_ptr[tb1a0_addr+3];
                    uint8_t boxflag = ram_ptr[0x0CB2];
                    uint8_t cb3val = ram_ptr[0x0CB3];
                    ESP_LOGW(TAG, "BOOT2-KEY: TB[$01F]=$%08lX TB[$1A0]=$%08lX TB[$1A7]=$%08lX",
                             (unsigned long)tb01f, (unsigned long)tb1a0, (unsigned long)tb1a7);
                    ESP_LOGW(TAG, "BOOT2-KEY: BoxFlag($0CB2)=$%02X MachByte($0CB3)=$%02X UNIMP=$%08lX",
                             boxflag, cb3val, (unsigned long)best_val);
                }

                /* Toolbox table: only patch ZERO entries to ROM's own handler.
                 * DO NOT redirect entries matching best_val ($400768) to $1800.
                 * Toolbox traps use a stack-based calling convention: the handler
                 * must pop parameters pushed by the caller.  Our simple safe
                 * handler at $1800 (SUBA.L A0,A0; MOVEQ #-1,D0; RTS) does NOT
                 * pop parameters, leaving stale data (e.g., resource type "ROv#")
                 * on the stack.  Subsequent RTS pops this data as a return address,
                 * crashing to $757400 — the root cause of the F631 crash. */
                int patched = 0;
                for (int i = 0; i < 512; i++) {
                    uint32_t addr = 0x0E00 + i * 4;
                    uint32_t entry = ((uint32_t)ram_ptr[addr]<<24)|
                                     ((uint32_t)ram_ptr[addr+1]<<16)|
                                     ((uint32_t)ram_ptr[addr+2]<<8)|
                                     ram_ptr[addr+3];
                    if (entry == 0 && best_val != 0) {
                        /* Zero entry: redirect to ROM's unimplemented handler
                         * to prevent JMP-to-zero crash.  ROM handler may bomb,
                         * but at least the stack stays clean. */
                        ram_ptr[addr]   = (best_val >> 24) & 0xFF;
                        ram_ptr[addr+1] = (best_val >> 16) & 0xFF;
                        ram_ptr[addr+2] = (best_val >> 8) & 0xFF;
                        ram_ptr[addr+3] = best_val & 0xFF;
                        patched++;
                    }
                }
                ESP_LOGW(TAG, "Patched %d zero Toolbox entries -> ROM handler", patched);

                /* Patch OS trap table at $0400 */
                uint32_t os_best = 0;
                int os_best_cnt = 0;
                for (int i = 0; i < 256; i++) {
                    uint32_t addr = 0x0400 + i * 4;
                    uint32_t entry = ((uint32_t)ram_ptr[addr]<<24)|
                                     ((uint32_t)ram_ptr[addr+1]<<16)|
                                     ((uint32_t)ram_ptr[addr+2]<<8)|
                                     ram_ptr[addr+3];
                    if (entry == 0) continue;
                    int count = 0;
                    for (int j = 0; j < 256; j++) {
                        uint32_t a2 = 0x0400 + j * 4;
                        uint32_t e2 = ((uint32_t)ram_ptr[a2]<<24)|
                                      ((uint32_t)ram_ptr[a2+1]<<16)|
                                      ((uint32_t)ram_ptr[a2+2]<<8)|
                                      ram_ptr[a2+3];
                        if (e2 == entry) count++;
                    }
                    if (count > os_best_cnt) { os_best_cnt = count; os_best = entry; }
                }
                /* Dump critical Memory Manager trap entries BEFORE patching.
                 * If any of these equal os_best ($400768), they'll be
                 * overwritten — which would break the Memory Manager. */
                {
                    static const struct { uint8_t idx; const char *name; } mm_traps[] = {
                        {0x1E,"NewPtr"}, {0x1F,"DisposPtr"}, {0x22,"NewHandle"},
                        {0x23,"DisposHandle"}, {0x24,"SetHandleSize"},
                        {0x25,"GetHandleSize"}, {0x29,"HLock"/*Compat*/},
                        {0x2E,"BlockMove"}, {0x40,"ResrvMem"}, {0x44,"MaxApplZone"},
                        {0x69,"HLock"}, {0x6A,"HUnlock"},
                    };
                    for (int t = 0; t < (int)(sizeof(mm_traps)/sizeof(mm_traps[0])); t++) {
                        uint32_t a = 0x0400 + mm_traps[t].idx * 4;
                        uint32_t v = ((uint32_t)ram_ptr[a]<<24)|((uint32_t)ram_ptr[a+1]<<16)|
                                     ((uint32_t)ram_ptr[a+2]<<8)|ram_ptr[a+3];
                        const char *tag = (v == os_best) ? " ***UNIMP***" :
                                          (v == 0) ? " ***ZERO***" : "";
                        ESP_LOGW(TAG, "  OS[$%02X] %s = $%08lX%s",
                                 mm_traps[t].idx, mm_traps[t].name,
                                 (unsigned long)v, tag);
                    }
                }
                int os_patched = 0;
                if (os_best_cnt > 5) {
                    for (int i = 0; i < 256; i++) {
                        uint32_t addr = 0x0400 + i * 4;
                        uint32_t entry = ((uint32_t)ram_ptr[addr]<<24)|
                                         ((uint32_t)ram_ptr[addr+1]<<16)|
                                         ((uint32_t)ram_ptr[addr+2]<<8)|
                                         ram_ptr[addr+3];
                        if (entry == 0) {
                            /* Only patch truly zero/null entries.
                             * DO NOT patch entries == os_best ($400768):
                             * the System file's GetTrapAddress checks for the ROM's
                             * unimplemented handler to decide whether to install its
                             * own implementation. Replacing $400768 with $000030
                             * tricks the System into thinking the trap is already
                             * handled, so it skips installing critical handlers
                             * (_Open, _Read, _FSDispatch, etc.). */
                            ram_ptr[addr]   = 0x00; ram_ptr[addr+1] = 0x00;
                            ram_ptr[addr+2] = 0x00; ram_ptr[addr+3] = 0x30;
                            os_patched++;
                        }
                    }
                }
                ESP_LOGW(TAG, "OS table: most common=$%08lX (unimp, NOT patched), zero->$30=%d/256",
                         (unsigned long)os_best, os_patched);

                /* Read and store Line-A handler address for trap ring buffer */
                line_a_handler_addr = ((uint32_t)ram_ptr[0x28]<<24)|
                    ((uint32_t)ram_ptr[0x29]<<16)|
                    ((uint32_t)ram_ptr[0x2A]<<8)|ram_ptr[0x2B];
                line_a_handler_addr &= 0xFFFFFF;
                ESP_LOGW(TAG, "Line-A handler=$%06X", line_a_handler_addr);

                /* === RESOURCE TRAP TRAMPOLINE ===
                 * Install 68000 code at $1FA500 that intercepts the Line-A
                 * handler. For resource traps (_GetResource $A9A0,
                 * _Get1Resource $A9A7, _CountResources $A99B,
                 * _GetIndResource $A99C), logs theType+theID to a circular
                 * buffer at $1FA000. Then JMPs to the real Line-A handler.
                 *
                 * On Line-A exception entry:
                 *   SP+0: SR(2), SP+2: saved_PC(4)
                 *   SP+6: theID(2), SP+8: theType(4) (for Toolbox Pascal calls)
                 *
                 * Log buffer layout at $1FA000:
                 *   $1FA000: next_ptr (4 bytes, init=$1FA008)
                 *   $1FA004: end_ptr  (4 bytes, const=$1FA500)
                 *   $1FA008..$1FA4FF: entries (8 bytes each, max 156)
                 *     [trap_word(2) + theType(4) + theID(2)]
                 */
                if (line_a_handler_addr != 0 && line_a_handler_addr < 0x480000) {
                    uint32_t la = line_a_handler_addr;
                    /* Initialize log buffer */
                    ram_ptr[0x1FA000]=0x00; ram_ptr[0x1FA001]=0x1F;
                    ram_ptr[0x1FA002]=0xA0; ram_ptr[0x1FA003]=0x08; /* next_ptr=$1FA008 */
                    ram_ptr[0x1FA004]=0x00; ram_ptr[0x1FA005]=0x1F;
                    ram_ptr[0x1FA006]=0xA5; ram_ptr[0x1FA007]=0x00; /* end_ptr=$1FA500 */
                    memset(ram_ptr + 0x1FA008, 0, 0x1FA500 - 0x1FA008); /* clear entries */

                    /* Write trampoline code at $1FA500
                     * Intercepts: $A9A0 _GetResource, $A9A7 _Get1Resource,
                     * $A99B _CountResources, $A99C _GetIndResource,
                     * $A81F (ROM's _Get1Resource variant used by boot 2) */
                    uint32_t c = 0x1FA500;
                    /* MOVEM.L D0/A0,-(SP)  48E7 8080 */
                    ram_ptr[c]=0x48; ram_ptr[c+1]=0xE7; ram_ptr[c+2]=0x80; ram_ptr[c+3]=0x80; c+=4;
                    /* MOVEA.L (10,SP),A0   206F 000A */
                    ram_ptr[c]=0x20; ram_ptr[c+1]=0x6F; ram_ptr[c+2]=0x00; ram_ptr[c+3]=0x0A; c+=4;
                    /* MOVE.W (A0),D0       3010 */
                    ram_ptr[c]=0x30; ram_ptr[c+1]=0x10; c+=2;
                    /* 5 comparisons × 6 bytes each = 30 bytes to pass_addr
                     * Each: CMPI.W(4) + BEQ.S(2) = 6 bytes
                     * BEQ displacement = bytes from BEQ+2 to log_addr */
                    /* CMPI.W #$A9A0,D0 */
                    ram_ptr[c]=0x0C; ram_ptr[c+1]=0x40; ram_ptr[c+2]=0xA9; ram_ptr[c+3]=0xA0; c+=4;
                    ram_ptr[c]=0x67; ram_ptr[c+1]=0x00; c+=2; /* BEQ.S log (patched later) */
                    /* CMPI.W #$A9A7,D0 */
                    ram_ptr[c]=0x0C; ram_ptr[c+1]=0x40; ram_ptr[c+2]=0xA9; ram_ptr[c+3]=0xA7; c+=4;
                    ram_ptr[c]=0x67; ram_ptr[c+1]=0x00; c+=2; /* BEQ.S log */
                    /* CMPI.W #$A99B,D0 */
                    ram_ptr[c]=0x0C; ram_ptr[c+1]=0x40; ram_ptr[c+2]=0xA9; ram_ptr[c+3]=0x9B; c+=4;
                    ram_ptr[c]=0x67; ram_ptr[c+1]=0x00; c+=2; /* BEQ.S log */
                    /* CMPI.W #$A99C,D0 */
                    ram_ptr[c]=0x0C; ram_ptr[c+1]=0x40; ram_ptr[c+2]=0xA9; ram_ptr[c+3]=0x9C; c+=4;
                    ram_ptr[c]=0x67; ram_ptr[c+1]=0x00; c+=2; /* BEQ.S log */
                    /* CMPI.W #$A81F,D0 (boot 2's _Get1Resource variant) */
                    ram_ptr[c]=0x0C; ram_ptr[c+1]=0x40; ram_ptr[c+2]=0xA8; ram_ptr[c+3]=0x1F; c+=4;
                    ram_ptr[c]=0x67; ram_ptr[c+1]=0x00; c+=2; /* BEQ.S log */
                    /* pass: MOVEM.L (SP)+,D0/A0  4CDF 0101 */
                    uint32_t pass_addr = c;
                    ram_ptr[c]=0x4C; ram_ptr[c+1]=0xDF; ram_ptr[c+2]=0x01; ram_ptr[c+3]=0x01; c+=4;
                    /* JMP $00XXXXXX (original Line-A handler) */
                    ram_ptr[c]=0x4E; ram_ptr[c+1]=0xF9;
                    ram_ptr[c+2]=(la>>24)&0xFF; ram_ptr[c+3]=(la>>16)&0xFF;
                    ram_ptr[c+4]=(la>>8)&0xFF;  ram_ptr[c+5]=la&0xFF; c+=6;
                    /* log: MOVE.L A1,-(SP)  2F09 */
                    uint32_t log_addr = c;
                    ram_ptr[c]=0x2F; ram_ptr[c+1]=0x09; c+=2;
                    /* MOVEA.L $001FA000,A1  2279 001F A000 */
                    ram_ptr[c]=0x22; ram_ptr[c+1]=0x79;
                    ram_ptr[c+2]=0x00; ram_ptr[c+3]=0x1F; ram_ptr[c+4]=0xA0; ram_ptr[c+5]=0x00; c+=6;
                    /* CMPA.L $001FA004,A1   B3F9 001F A004 */
                    ram_ptr[c]=0xB3; ram_ptr[c+1]=0xF9;
                    ram_ptr[c+2]=0x00; ram_ptr[c+3]=0x1F; ram_ptr[c+4]=0xA0; ram_ptr[c+5]=0x04; c+=6;
                    /* BGE.S skip (+$10)    6C10 */
                    ram_ptr[c]=0x6C; ram_ptr[c+1]=0x10; c+=2;
                    /* MOVE.W D0,(A1)+      32C0 */
                    ram_ptr[c]=0x32; ram_ptr[c+1]=0xC0; c+=2;
                    /* MOVE.L (20,SP),(A1)+  22EF 0014 */
                    ram_ptr[c]=0x22; ram_ptr[c+1]=0xEF; ram_ptr[c+2]=0x00; ram_ptr[c+3]=0x14; c+=4;
                    /* MOVE.W (18,SP),(A1)+  32EF 0012 */
                    ram_ptr[c]=0x32; ram_ptr[c+1]=0xEF; ram_ptr[c+2]=0x00; ram_ptr[c+3]=0x12; c+=4;
                    /* MOVE.L A1,$001FA000   23C9 001F A000 */
                    ram_ptr[c]=0x23; ram_ptr[c+1]=0xC9;
                    ram_ptr[c+2]=0x00; ram_ptr[c+3]=0x1F; ram_ptr[c+4]=0xA0; ram_ptr[c+5]=0x00; c+=6;
                    /* skip: MOVEA.L (SP)+,A1  225F */
                    ram_ptr[c]=0x22; ram_ptr[c+1]=0x5F; c+=2;
                    /* BRA.S pass  60 XX */
                    int8_t disp = (int8_t)((int32_t)pass_addr - (int32_t)(c + 2));
                    ram_ptr[c]=0x60; ram_ptr[c+1]=(uint8_t)disp; c+=2;

                    /* Verify branch displacements */
                    ESP_LOGW(TAG, "RES-TRAP: trampoline at $%06X-%06X, log at $1FA008, pass=$%06X disp=%d",
                             0x1FA500, c - 1, pass_addr, (int)disp);

                    /* Patch BEQ.S displacements for all 5 comparisons.
                     * Layout from $1FA500:
                     *   +$00: MOVEM(4) + MOVEA(4) + MOVE.W(2) = 10 bytes
                     *   +$0A: CMP1(4)+BEQ(2) = +$0E is BEQ#1
                     *   +$10: CMP2(4)+BEQ(2) = +$14 is BEQ#2
                     *   +$16: CMP3(4)+BEQ(2) = +$1A is BEQ#3
                     *   +$1C: CMP4(4)+BEQ(2) = +$20 is BEQ#4
                     *   +$22: CMP5(4)+BEQ(2) = +$26 is BEQ#5
                     *   +$28: pass (MOVEM + JMP) */
                    uint32_t beq_addrs[5] = {
                        0x1FA50E, 0x1FA514, 0x1FA51A, 0x1FA520, 0x1FA526
                    };
                    for (int bi = 0; bi < 5; bi++) {
                        int bd = (int)(log_addr - (beq_addrs[bi] + 2));
                        ram_ptr[beq_addrs[bi] + 1] = (uint8_t)bd;
                    }
                    /* Fix the BGE.S displacement for skip.
                     * After log: entry, the code is:
                     * log+0: MOVE.L A1,-(SP) (2)
                     * log+2: MOVEA.L abs,A1 (6)
                     * log+8: CMPA.L abs,A1 (6)
                     * log+14: BGE.S skip (2)
                     * log+16: MOVE.W D0,(A1)+ (2)
                     * log+18: MOVE.L (20,SP),(A1)+ (4)
                     * log+22: MOVE.W (18,SP),(A1)+ (4)
                     * log+26: MOVE.L A1,abs (6)
                     * skip = log+32: MOVEA.L (SP)+,A1 (2)
                     * log+34: BRA.S pass (2)
                     */
                    uint32_t skip_addr = log_addr + 32;
                    int bge_disp = (int)(skip_addr - (log_addr + 14 + 2));
                    ram_ptr[log_addr + 14 + 1] = (uint8_t)bge_disp;

                    /* Redirect Line-A vector ($0028) to trampoline */
                    ram_ptr[0x28] = 0x00;
                    ram_ptr[0x29] = (0x1FA500 >> 16) & 0xFF;
                    ram_ptr[0x2A] = (0x1FA500 >> 8) & 0xFF;
                    ram_ptr[0x2B] = 0x1FA500 & 0xFF;
                    ESP_LOGW(TAG, "RES-TRAP: Line-A vector -> $%06X (was $%06X)",
                             0x1FA500, line_a_handler_addr);
                    /* Update line_a_handler_addr for existing monitors */
                    line_a_handler_addr = 0x1FA500;
                }

                /* Dump heap zone state */
                {
                    uint32_t syszone  = ((uint32_t)ram_ptr[0x2A6]<<24)|((uint32_t)ram_ptr[0x2A7]<<16)|
                                        ((uint32_t)ram_ptr[0x2A8]<<8)|ram_ptr[0x2A9];
                    uint32_t applzone = ((uint32_t)ram_ptr[0x2AA]<<24)|((uint32_t)ram_ptr[0x2AB]<<16)|
                                        ((uint32_t)ram_ptr[0x2AC]<<8)|ram_ptr[0x2AD];
                    uint32_t thezone  = ((uint32_t)ram_ptr[0x118]<<24)|((uint32_t)ram_ptr[0x119]<<16)|
                                        ((uint32_t)ram_ptr[0x11A]<<8)|ram_ptr[0x11B];
                    uint32_t heapend  = ((uint32_t)ram_ptr[0x114]<<24)|((uint32_t)ram_ptr[0x115]<<16)|
                                        ((uint32_t)ram_ptr[0x116]<<8)|ram_ptr[0x117];
                    ESP_LOGW(TAG, "Heap: SysZone=$%08lX ApplZone=$%08lX TheZone=$%08lX HeapEnd=$%08lX",
                             (unsigned long)syszone, (unsigned long)applzone,
                             (unsigned long)thezone, (unsigned long)heapend);
                }

                /* === Safe handler and recovery loop ===
                 * Placed at $0030-$0039 in the RESERVED 68000 vector area.
                 * The 68000 never generates exceptions for vectors $0030-$005F.
                 * Previously at $1800 which is INSIDE the system heap
                 * (SysZone=$1600) — the Memory Manager overwrote it with heap
                 * block data during resource loading, causing cascading
                 * corruption (BlockMove with bad pointers, vector trashing).
                 * Returns D0=-1 (error) and A0=0 (NULL). */
                ram_ptr[0x0030] = 0x91; ram_ptr[0x0031] = 0xC8;  /* SUBA.L A0,A0 */
                ram_ptr[0x0032] = 0x70; ram_ptr[0x0033] = 0xFF;  /* MOVEQ #-1,D0 */
                ram_ptr[0x0034] = 0x4E; ram_ptr[0x0035] = 0x75;  /* RTS */

                /* Recovery tight loop at $0036 (used by crash recovery) */
                ram_ptr[0x0036] = 0x4E; ram_ptr[0x0037] = 0x71;  /* NOP */
                ram_ptr[0x0038] = 0x60; ram_ptr[0x0039] = 0xFC;  /* BRA.S $0036 */

                /* Exception handlers: all redirect to $001800 (MOVEQ #0,D0; RTS).
                 * This makes the faulting function "return 0" cleanly.
                 *
                 * Bus/Address error entry at $2000:
                 *   ADDQ.L  #8,SP                ; strip extra 8 bytes (FC+addr+IR)
                 * Shared entry at $2002 (for ILLEGAL, ZERO-DIV, PRIV-VIOL):
                 *   MOVE.L  #$001800,(2,SP)      ; replace saved PC with safe handler
                 *   MOVEQ   #0,D0                ; clear D0
                 *   RTE                           ; pop SR, go to $1800 → RTS to caller
                 *
                 * ILLEGAL/ZERO-DIV/PRIV-VIOL at $2010/$2014/$2018: BRA.S $2002 */
                ram_ptr[0x2000] = 0x50; ram_ptr[0x2001] = 0x8F;  /* ADDQ.L #8,SP */
                ram_ptr[0x2002] = 0x2F; ram_ptr[0x2003] = 0x7C;  /* MOVE.L #imm,(d16,SP) */
                ram_ptr[0x2004] = 0x00; ram_ptr[0x2005] = 0x00;  /* imm high = $0000 */
                ram_ptr[0x2006] = 0x00; ram_ptr[0x2007] = 0x30;  /* imm low  = $0030 */
                ram_ptr[0x2008] = 0x00; ram_ptr[0x2009] = 0x02;  /* d16 = 2 */
                ram_ptr[0x200A] = 0x70; ram_ptr[0x200B] = 0x00;  /* MOVEQ #0,D0 */
                ram_ptr[0x200C] = 0x4E; ram_ptr[0x200D] = 0x73;  /* RTE */
                ram_ptr[0x200E] = 0x4E; ram_ptr[0x200F] = 0x71;  /* NOP (padding) */
                ram_ptr[0x2010] = 0x60; ram_ptr[0x2011] = 0xF0;  /* BRA.S $2002 (ILLEGAL) */
                ram_ptr[0x2012] = 0x4E; ram_ptr[0x2013] = 0x71;  /* NOP (padding) */
                ram_ptr[0x2014] = 0x60; ram_ptr[0x2015] = 0xEC;  /* BRA.S $2002 (ZERO-DIV) */
                ram_ptr[0x2016] = 0x4E; ram_ptr[0x2017] = 0x71;  /* NOP (padding) */
                ram_ptr[0x2018] = 0x60; ram_ptr[0x2019] = 0xE8;  /* BRA.S $2002 (PRIV-VIOL) */

                /* Line-F handler at $2020: when executing unmapped memory
                 * ($FFFF = Line-F), the stack is likely corrupted.  Redirect
                 * to $1800 which does SUBA.L A0,A0; MOVEQ #-1,D0; RTS.
                 * If SP is already very low (below $2000), we've overflowed —
                 * force a soft reset by setting SP and jumping to boot loop.
                 *
                 * $2020: CMPA.L  #$002000,SP   ; 6 bytes
                 * $2026: BCC.S   $202E          ; if SP >= $2000, do normal redirect
                 * $2028: MOVE.L  #$0FFA00,SP    ; reset stack
                 * $202E: same shared handler at $2002 */
                ram_ptr[0x2020] = 0xBF; ram_ptr[0x2021] = 0xFC;  /* CMPA.L #imm,SP */
                ram_ptr[0x2022] = 0x00; ram_ptr[0x2023] = 0x00;
                ram_ptr[0x2024] = 0x20; ram_ptr[0x2025] = 0x00;  /* imm = $00002000 */
                ram_ptr[0x2026] = 0x64; ram_ptr[0x2027] = 0x06;  /* BCC.S $202E */
                ram_ptr[0x2028] = 0x2E; ram_ptr[0x2029] = 0x7C;  /* MOVEA.L #imm,SP */
                ram_ptr[0x202A] = 0x00; ram_ptr[0x202B] = 0x0F;
                ram_ptr[0x202C] = 0xFA; ram_ptr[0x202D] = 0x00;  /* imm = $000FFA00 */
                ram_ptr[0x202E] = 0x60; ram_ptr[0x202F] = 0xD2;  /* BRA.S $2002 */

                /* Install exception vectors */
                ram_ptr[0x08]=0x00; ram_ptr[0x09]=0x00; ram_ptr[0x0A]=0x20; ram_ptr[0x0B]=0x00;
                ram_ptr[0x0C]=0x00; ram_ptr[0x0D]=0x00; ram_ptr[0x0E]=0x20; ram_ptr[0x0F]=0x00;
                ram_ptr[0x10]=0x00; ram_ptr[0x11]=0x00; ram_ptr[0x12]=0x20; ram_ptr[0x13]=0x10;
                ram_ptr[0x14]=0x00; ram_ptr[0x15]=0x00; ram_ptr[0x16]=0x20; ram_ptr[0x17]=0x14;
                ram_ptr[0x20]=0x00; ram_ptr[0x21]=0x00; ram_ptr[0x22]=0x20; ram_ptr[0x23]=0x18;
                /* Line-F vector ($002C) → $002020 (stack overflow protection) */
                ram_ptr[0x2C]=0x00; ram_ptr[0x2D]=0x00; ram_ptr[0x2E]=0x20; ram_ptr[0x2F]=0x20;
                ESP_LOGW(TAG, "Exc vectors installed (incl Line-F)");

                /* NOTE: $0360-$0369 is NOT the I/O queue — it overlaps the
                 * Deferred Task Queue (DTQueue).  Zeroing it corrupted
                 * ROM data structures and caused address errors.  Removed. */

                /* === Install drive in Mac drive queue ===
                 * DrvQHdr at $0308: qFlags(2) qHead(4) qTail(4).
                 *
                 * CRITICAL: The DrvQEl MUST NOT be inside the system heap
                 * (SysZone=$1600).  Previous location at $1700 destroyed
                 * the Memory Manager's free master pointer chain — the
                 * heap dump showed sequential back-pointers at $16E0-$171F
                 * that we were overwriting with DrvSts/DrvQEl data.  This
                 * caused _NewHandle to follow a corrupted chain, producing
                 * garbage pointers passed to _BlockMove → vector trashing.
                 *
                 * New location: $0040-$0057 — 68000 vector slots #16-21
                 * ("Unassigned, Reserved").  The 68000 never generates
                 * these exceptions.  qLink is at $0046. */
                {
                    uint32_t old_head = ((uint32_t)ram_ptr[0x30A]<<24)|
                                        ((uint32_t)ram_ptr[0x30B]<<16)|
                                        ((uint32_t)ram_ptr[0x30C]<<8)|ram_ptr[0x30D];
                    uint32_t old_tail = ((uint32_t)ram_ptr[0x30E]<<24)|
                                        ((uint32_t)ram_ptr[0x30F]<<16)|
                                        ((uint32_t)ram_ptr[0x310]<<8)|ram_ptr[0x311];
                    ESP_LOGW(TAG, "DrvQ before: head=%08lX tail=%08lX",
                             (unsigned long)old_head, (unsigned long)old_tail);

                    /* DrvSts + DrvQEl at $000040 (24 bytes, $0040-$0057).
                     * DrvSts layout (offsets from qLink at $0046):
                     *   -6: track       (word)  = 0        @ $0040
                     *   -4: writeProt   (byte)  = 0        @ $0042
                     *   -3: diskInPlace (byte)  = 2 (HD)   @ $0043
                     *   -2: installed   (byte)  = FF (-1)  @ $0044
                     *   -1: sides       (byte)  = FF (-1)  @ $0045
                     *    0: qLink       (long)  = 0        @ $0046
                     *   +4: qType       (word)  = 1        @ $004A
                     *   +6: dQDrive     (word)  = 1        @ $004C
                     *   +8: dQRefNum    (word)  = -5       @ $004E
                     *  +10: dQFSID      (word)  = 0        @ $0050
                     *  +12: twoSideFmt  (word)  = -1       @ $0052
                     *  +14: needsFlush  (word)  = 0        @ $0054
                     *  +16: diskErrs    (word)  = 0        @ $0056
                     */
                    ram_ptr[0x0040]=0x00; ram_ptr[0x0041]=0x00; /* track=0 */
                    /* writeProt bit 7: the image is in flash (read-only).
                     * The File Manager then mounts the volume locked and
                     * never writes. Unlocked, .Sony _Write calls reached the
                     * bypassed ROM driver: sync ones "succeeded" (the Prime
                     * intercept copied disk data INTO the write buffer) and
                     * async ones (Desktop Manager on volumes >= 2 MB) never
                     * completed, hanging the File Manager. */
                    /* writable disk (mac_disk_overlay.c): unlocked when the PSRAM overlay is active */
                    ram_ptr[0x0042]=iwm_disk_writable ? 0x00 : 0x80; /* writeProt */
                    ram_ptr[0x0043]=0x02;                       /* diskInPlace=2 */
                    ram_ptr[0x0044]=0xFF;                       /* installed=-1 */
                    ram_ptr[0x0045]=0xFF;                       /* sides=-1 */
                    ram_ptr[0x0046]=0; ram_ptr[0x0047]=0;
                    ram_ptr[0x0048]=0; ram_ptr[0x0049]=0;       /* qLink=0 */
                    ram_ptr[0x004A]=0x00; ram_ptr[0x004B]=0x01; /* qType=1 */
                    ram_ptr[0x004C]=0x00; ram_ptr[0x004D]=0x01; /* dQDrive=1 */
                    ram_ptr[0x004E]=0xFF; ram_ptr[0x004F]=0xFB; /* dQRefNum=-5 */
                    ram_ptr[0x0050]=0x00; ram_ptr[0x0051]=0x00; /* dQFSID=0 */
                    ram_ptr[0x0052]=0xFF; ram_ptr[0x0053]=0xFF; /* twoSideFmt=-1 */
                    ram_ptr[0x0054]=0x00; ram_ptr[0x0055]=0x00; /* needsFlush=0 */
                    ram_ptr[0x0056]=0x00; ram_ptr[0x0057]=0x00; /* diskErrs=0 */

                    if (old_head == 0) {
                        /* Empty queue — qHead/qTail point to qLink */
                        ram_ptr[0x30A]=0x00; ram_ptr[0x30B]=0x00;
                        ram_ptr[0x30C]=0x00; ram_ptr[0x30D]=0x46; /* qHead=$000046 */
                        ram_ptr[0x30E]=0x00; ram_ptr[0x30F]=0x00;
                        ram_ptr[0x310]=0x00; ram_ptr[0x311]=0x46; /* qTail=$000046 */
                    } else {
                        /* Append to existing chain */
                        uint32_t tm = old_tail % mac_mem_get_ram_size();
                        ram_ptr[tm]  =0x00; ram_ptr[tm+1]=0x00;
                        ram_ptr[tm+2]=0x00; ram_ptr[tm+3]=0x46;
                        ram_ptr[0x30E]=0x00; ram_ptr[0x30F]=0x00;
                        ram_ptr[0x310]=0x00; ram_ptr[0x311]=0x46;
                    }
                    ESP_LOGW(TAG, "DrvQ installed: drv=1 ref=-5 dip=2 @$0046");
                }

                /* Diagnose Unit Table to check .SONY driver installation.
                 * UTableBase at $011C, unit #4 = refNum -5 (.SONY).
                 * Also check the OS trap dispatch for $A00F (_MountVol). */
                {
                    uint32_t utBase = ((uint32_t)ram_ptr[0x11C]<<24)|
                                      ((uint32_t)ram_ptr[0x11D]<<16)|
                                      ((uint32_t)ram_ptr[0x11E]<<8)|
                                      ram_ptr[0x11F];
                    ESP_LOGW(TAG, "UTableBase=$%08lX", (unsigned long)utBase);
                    uint32_t rsz = mac_mem_get_ram_size();
                    if (utBase && utBase < rsz) {
                        /* Unit 4 = refNum -5 (.SONY) */
                        uint32_t um = utBase % rsz;
                        for (int u = 0; u < 8; u++) {
                            uint32_t off = um + u * 4;
                            if (off + 3 < rsz) {
                                uint32_t dce = ((uint32_t)ram_ptr[off]<<24)|
                                               ((uint32_t)ram_ptr[off+1]<<16)|
                                               ((uint32_t)ram_ptr[off+2]<<8)|
                                               ram_ptr[off+3];
                                if (dce) {
                                    ESP_LOGW(TAG, "  Unit[%d] ref=%d DCE=$%08lX",
                                             u, -(u+1), (unsigned long)dce);
                                }
                            }
                        }
                    }
                    /* OS trap $0F (_MountVol) handler address */
                    uint32_t trap_0f = ((uint32_t)ram_ptr[0x43C]<<24)|
                                       ((uint32_t)ram_ptr[0x43D]<<16)|
                                       ((uint32_t)ram_ptr[0x43E]<<8)|
                                       ram_ptr[0x43F];
                    ESP_LOGW(TAG, "Trap $A00F handler=$%08lX", (unsigned long)trap_0f);

                    /* Log Device Manager trap handler addresses */
                    uint32_t trap_02 = ((uint32_t)ram_ptr[0x408]<<24)|((uint32_t)ram_ptr[0x409]<<16)|
                                       ((uint32_t)ram_ptr[0x40A]<<8)|ram_ptr[0x40B];
                    uint32_t trap_03 = ((uint32_t)ram_ptr[0x40C]<<24)|((uint32_t)ram_ptr[0x40D]<<16)|
                                       ((uint32_t)ram_ptr[0x40E]<<8)|ram_ptr[0x40F];
                    uint32_t trap_04 = ((uint32_t)ram_ptr[0x410]<<24)|((uint32_t)ram_ptr[0x411]<<16)|
                                       ((uint32_t)ram_ptr[0x412]<<8)|ram_ptr[0x413];
                    uint32_t trap_05 = ((uint32_t)ram_ptr[0x414]<<24)|((uint32_t)ram_ptr[0x415]<<16)|
                                       ((uint32_t)ram_ptr[0x416]<<8)|ram_ptr[0x417];
                    ESP_LOGW(TAG, "Traps: _Read=$%08lX _Write=$%08lX _Ctl=$%08lX _Stat=$%08lX",
                             (unsigned long)trap_02, (unsigned long)trap_03,
                             (unsigned long)trap_04, (unsigned long)trap_05);
                    /* Raw bytes at DCE handle ($1738) and nearby */
                    ESP_LOGW(TAG, "DCE handle @$1738: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                             ram_ptr[0x1738],ram_ptr[0x1739],ram_ptr[0x173A],ram_ptr[0x173B],
                             ram_ptr[0x173C],ram_ptr[0x173D],ram_ptr[0x173E],ram_ptr[0x173F],
                             ram_ptr[0x1740],ram_ptr[0x1741],ram_ptr[0x1742],ram_ptr[0x1743]);

                    /* Read .SONY DCE via proper Handle double-dereference.
                     * Unit[4] = Handle (addr of master pointer).
                     * *Handle = master pointer (flags in high byte, addr in low 3).
                     * **Handle = DCE block.  dCtlDriver is DCE+0 (long ptr). */
                    if (utBase && utBase < rsz) {
                        uint32_t unit4_off = (utBase % rsz) + 4 * 4; /* unit 4 */
                        if (unit4_off + 3 < rsz) {
                            uint32_t handle = ((uint32_t)ram_ptr[unit4_off]<<24)|
                                              ((uint32_t)ram_ptr[unit4_off+1]<<16)|
                                              ((uint32_t)ram_ptr[unit4_off+2]<<8)|
                                              ram_ptr[unit4_off+3];
                            uint32_t hm = handle & 0xFFFFFF;
                            if (handle && hm < rsz) {
                                uint32_t master = ((uint32_t)ram_ptr[hm]<<24)|
                                                  ((uint32_t)ram_ptr[hm+1]<<16)|
                                                  ((uint32_t)ram_ptr[hm+2]<<8)|
                                                  ram_ptr[hm+3];
                                uint32_t dce_addr = master & 0xFFFFFF; /* strip MM flags */
                                ESP_LOGW(TAG, "SONY Handle=$%08lX Master=$%08lX DCE=$%06lX",
                                         (unsigned long)handle, (unsigned long)master,
                                         (unsigned long)dce_addr);
                                if (dce_addr && dce_addr < rsz) {
                                    uint32_t dm = dce_addr % rsz;
                                    uint32_t drvr_ptr = ((uint32_t)ram_ptr[dm]<<24)|
                                                        ((uint32_t)ram_ptr[dm+1]<<16)|
                                                        ((uint32_t)ram_ptr[dm+2]<<8)|
                                                        ram_ptr[dm+3];
                                    uint16_t dce_flags = ((uint16_t)ram_ptr[dm+4]<<8)|ram_ptr[dm+5];
                                    int16_t dce_ref = (int16_t)(((uint16_t)ram_ptr[dm+24]<<8)|ram_ptr[dm+25]);
                                    ESP_LOGW(TAG, "  dCtlDriver=$%08lX flags=$%04X ref=%d",
                                             (unsigned long)drvr_ptr, dce_flags, dce_ref);
                                    /* Read driver header offsets from drvr_ptr (in ROM) */
                                    uint32_t dp = drvr_ptr & 0xFFFFFF;
                                    uint16_t d_open  = (m68k_read_memory_8(dp+8)<<8)|m68k_read_memory_8(dp+9);
                                    uint16_t d_prime = (m68k_read_memory_8(dp+10)<<8)|m68k_read_memory_8(dp+11);
                                    uint16_t d_ctl   = (m68k_read_memory_8(dp+12)<<8)|m68k_read_memory_8(dp+13);
                                    uint16_t d_stat  = (m68k_read_memory_8(dp+14)<<8)|m68k_read_memory_8(dp+15);
                                    uint16_t d_close = (m68k_read_memory_8(dp+16)<<8)|m68k_read_memory_8(dp+17);
                                    ESP_LOGW(TAG, "  routines: open=$%06lX prime=$%06lX ctl=$%06lX stat=$%06lX close=$%06lX",
                                             (unsigned long)(dp + d_open), (unsigned long)(dp + d_prime),
                                             (unsigned long)(dp + d_ctl), (unsigned long)(dp + d_stat),
                                             (unsigned long)(dp + d_close));
                                }
                            }
                        }
                    }
                }

                /* Log IWM base pointer at $01D4. */
                {
                    uint32_t v1d4 = ((uint32_t)ram_ptr[0x1D4]<<24)|
                                    ((uint32_t)ram_ptr[0x1D5]<<16)|
                                    ((uint32_t)ram_ptr[0x1D6]<<8)|
                                    ram_ptr[0x1D7];
                    ESP_LOGW(TAG, "IWM $01D4=%08lX (ISM routing active)", (unsigned long)v1d4);
                }

                /* Force $0172 (BootState) = $FF so the gate check at sub5
                 * ($400DE8) passes immediately when the NOP bypass at $400CAC
                 * forces the boot loop to fall through.  Without this, sub5
                 * blocks ($0172=$80 = "waiting for disk"), and the intervening
                 * event processing at $400DCA uses garbage data, causing a
                 * bus error at $402AD0 that dumps the CPU into the dispatch loop. */
                ram_ptr[0x172] = 0xFF;
                ESP_LOGW(TAG, "PATCH: $0172=FF (force boot gate open)");

                /* Set BootDrive ($0210) to drive 1 (our floppy).
                 * The ROM's boot code may not set this correctly because
                 * our NOP bypass at $400CAC skips event processing that
                 * would normally determine the boot drive. Without this,
                 * _Launch can't find the Finder on the boot volume. */
                {
                    uint16_t old_bd = (ram_ptr[0x210]<<8)|ram_ptr[0x211];
                    ram_ptr[0x210] = 0x00; ram_ptr[0x211] = 0x01; /* drive 1 */
                    ESP_LOGW(TAG, "PATCH: BootDrive=$0001 (was $%04X)", old_bd);
                }

                /* Clear RAM-test residue from Window Manager globals.
                 * Mini vMac bypasses the RAM test entirely (ROM offsets $86
                 * and $11E), so WMgrPort and friends stay zero from calloc.
                 * Our emulator lets the RAM test run, which writes $FF to
                 * all bytes. The ROM clears MOST of RAM afterward, but
                 * leaves $09DE+ with the test pattern ($FFFFFFFF).
                 * The boot code ('boot' 2) checks WMgrPort != 0 to decide
                 * whether the Toolbox is already initialized. With
                 * WMgrPort=$FFFFFFFF it skips _InitWindows → WMgrPort
                 * never gets a real GrafPort → _Launch is never called.
                 * Clear these to zero so boot code properly initializes. */
                {
                    uint32_t old_wm = ((uint32_t)ram_ptr[0x9DE]<<24)|
                                      ((uint32_t)ram_ptr[0x9DF]<<16)|
                                      ((uint32_t)ram_ptr[0x9E0]<<8)|ram_ptr[0x9E1];
                    ram_ptr[0x9DE] = 0; ram_ptr[0x9DF] = 0;
                    ram_ptr[0x9E0] = 0; ram_ptr[0x9E1] = 0;  /* WMgrPort */
                    ram_ptr[0x9E2] = 0; ram_ptr[0x9E3] = 0;
                    ram_ptr[0x9E4] = 0; ram_ptr[0x9E5] = 0;  /* DeskPort */
                    ram_ptr[0xA1C] = 0; ram_ptr[0xA1D] = 0;
                    ram_ptr[0xA1E] = 0; ram_ptr[0xA1F] = 0;  /* GrayRgn */
                    ESP_LOGW(TAG, "PATCH: WMgrPort=$0 (was $%08lX)", (unsigned long)old_wm);
                }

                /* Initialize the parameter block at $0FFC00 for the boot _Read.
                 * The NOP bypass at $400CAC skips event processing that would
                 * normally populate these fields.  Without proper values, the
                 * _Read gets garbage ioReqCount ($6DB6DB6D = memtest pattern).
                 *   ioRefNum ($18) = -5 — patched by intercept at $400DF4
                 *   ioBuffer ($20) = will be set from A6 by ROM code
                 *   ioReqCount ($24) = 1024 ($0400) — boot block size
                 *   ioPosOffset ($2E) = 0 — start of disk */
                uint32_t pb = 0x0FFC00;
                ram_ptr[pb+0x24] = 0x00; ram_ptr[pb+0x25] = 0x00;
                ram_ptr[pb+0x26] = 0x04; ram_ptr[pb+0x27] = 0x00; /* ioReqCount=1024 */
                ram_ptr[pb+0x2E] = 0x00; ram_ptr[pb+0x2F] = 0x00;
                ram_ptr[pb+0x30] = 0x00; ram_ptr[pb+0x31] = 0x00; /* ioPosOffset=0 */
                /* Clear ioCompletion */
                ram_ptr[pb+0x0C] = 0x00; ram_ptr[pb+0x0D] = 0x00;
                ram_ptr[pb+0x0E] = 0x00; ram_ptr[pb+0x0F] = 0x00;
                /* Clear ioResult so poll doesn't see stale error */
                ram_ptr[pb+0x10] = 0x00; ram_ptr[pb+0x11] = 0x00;

                /* Sync internal SRAM shadow after all low-memory patches */
                mac_mem_sync_fast_ram();
                ESP_LOGW(TAG, "FAST-RAM synced after F600 patches");

                /* Enable ADB shift register interrupt emulation now that
                 * boot has passed the SCC handshake phase */
                via_set_adb_active(true);
            }
        }

        /* Disk insertion at F650: after ALL patches at F600.
         * Boot block loads ~170 frames later with all safety nets active. */
        if (frame_count == 650) {
            iwm_insert_disk();
        }

        /* Sense poll diagnostics: after disk insertion, check if VBL task
         * is polling the disk-in-place sense line. */
        if (frame_count == 750 || frame_count == 900 ||
            frame_count == 1200 || frame_count == 1500 ||
            frame_count == 2000 || frame_count == 3000 ||
            frame_count == 4000 || frame_count == 5000 ||
            frame_count == 6000 || frame_count == 7000 ||
            frame_count == 8000 || frame_count == 10000) {
            uint8_t *ram_diag = mac_mem_get_ram();
            uint8_t v0172 = (ram_diag) ? ram_diag[0x172] : 0xFF;
            uint32_t diag_pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            uint32_t diag_sp = m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF;
            uint32_t diag_a5 = m68k_get_reg(NULL, M68K_REG_A5) & 0xFFFFFF;
            uint32_t diag_a6 = m68k_get_reg(NULL, M68K_REG_A6) & 0xFFFFFF;
            ESP_LOGW(TAG, "DIAG F%d: $172=%02X PC=%06X SP=%06X A5=%06X A6=%06X sense=%d ism=%d",
                     frame_count, v0172, diag_pc, diag_sp, diag_a5, diag_a6,
                     iwm_get_sense_poll_count(),
                     iwm_get_sense_poll_ism());
            if (frame_count == 1200 && ram_diag) {
                for (int i = 0; i < 8; i++) {
                    uint32_t v = ((uint32_t)ram_diag[0x28000+i*4]<<24)|((uint32_t)ram_diag[0x28001+i*4]<<16)|
                                 ((uint32_t)ram_diag[0x28002+i*4]<<8)|ram_diag[0x28003+i*4];
                    ESP_LOGW(TAG, "OSVEC F%d $%04X: $%08lX", frame_count, 0x28000+i*4, (unsigned long)v);
                }
                for (int i = 0; i < 8; i++) {
                    uint32_t v = ((uint32_t)ram_diag[0x0E00+i*4]<<24)|((uint32_t)ram_diag[0x0E01+i*4]<<16)|
                                 ((uint32_t)ram_diag[0x0E02+i*4]<<8)|ram_diag[0x0E03+i*4];
                    ESP_LOGW(TAG, "TBVEC F%d $%04X: $%08lX", frame_count, 0x0E00+i*4, (unsigned long)v);
                }
                for (uint32_t a = 0x2F1C; a < 0x3150; a += 16) {
                    ESP_LOGW(TAG, "BOOT3-CODE $%04X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                             a,
                             ram_diag[a],     ram_diag[a+1],  ram_diag[a+2],  ram_diag[a+3],
                             ram_diag[a+4],   ram_diag[a+5],  ram_diag[a+6],  ram_diag[a+7],
                             ram_diag[a+8],   ram_diag[a+9],  ram_diag[a+10], ram_diag[a+11],
                             ram_diag[a+12],  ram_diag[a+13], ram_diag[a+14], ram_diag[a+15]);
                }
            }
            /* Dump key low-memory globals */
            if (ram_diag) {
                uint32_t memtop   = ((uint32_t)ram_diag[0x108]<<24)|((uint32_t)ram_diag[0x109]<<16)|
                                    ((uint32_t)ram_diag[0x10A]<<8)|ram_diag[0x10B];
                uint32_t bufptr   = ((uint32_t)ram_diag[0x10C]<<24)|((uint32_t)ram_diag[0x10D]<<16)|
                                    ((uint32_t)ram_diag[0x10E]<<8)|ram_diag[0x10F];
                uint32_t scrnbase = ((uint32_t)ram_diag[0x824]<<24)|((uint32_t)ram_diag[0x825]<<16)|
                                    ((uint32_t)ram_diag[0x826]<<8)|ram_diag[0x827];
                uint32_t stkbase  = ((uint32_t)ram_diag[0x908]<<24)|((uint32_t)ram_diag[0x909]<<16)|
                                    ((uint32_t)ram_diag[0x90A]<<8)|ram_diag[0x90B];
                uint32_t heapend  = ((uint32_t)ram_diag[0x114]<<24)|((uint32_t)ram_diag[0x115]<<16)|
                                    ((uint32_t)ram_diag[0x116]<<8)|ram_diag[0x117];
                ESP_LOGW(TAG, "  MemTop=$%08lX BufPtr=$%08lX ScrnBase=$%08lX StkBase=$%08lX HeapEnd=$%08lX",
                         (unsigned long)memtop, (unsigned long)bufptr,
                         (unsigned long)scrnbase, (unsigned long)stkbase,
                         (unsigned long)heapend);
                /* ResErr($0A60), MemErr($0220), DSErrCode($0AF0) */
                {
                    int16_t res_err = (int16_t)((ram_diag[0xA60]<<8)|ram_diag[0xA61]);
                    int16_t mem_err = (int16_t)((ram_diag[0x220]<<8)|ram_diag[0x221]);
                    int16_t ds_err  = (int16_t)((ram_diag[0xAF0]<<8)|ram_diag[0xAF1]);
                    uint32_t topmap = ((uint32_t)ram_diag[0xA50]<<24)|((uint32_t)ram_diag[0xA51]<<16)|
                                      ((uint32_t)ram_diag[0xA52]<<8)|ram_diag[0xA53];
                    uint32_t sysmap = ((uint32_t)ram_diag[0xA54]<<24)|((uint32_t)ram_diag[0xA55]<<16)|
                                      ((uint32_t)ram_diag[0xA56]<<8)|ram_diag[0xA57];
                    uint8_t boxflag = ram_diag[0xCB2];
                    ESP_LOGW(TAG, "  ResErr=%d MemErr=%d DSErr=%d TopMap=$%08lX SysMap=$%08lX Box=$%02X",
                             res_err, mem_err, ds_err,
                             (unsigned long)topmap, (unsigned long)sysmap, boxflag);
                }
                /* Check framebuffer for content: read a few bytes at ScrnBase */
                uint32_t sb = scrnbase & 0xFFFFFF;
                uint32_t sbm = sb % mac_mem_get_ram_size();
                if (sbm + 100 < mac_mem_get_ram_size()) {
                    uint32_t w0 = ((uint32_t)ram_diag[sbm]<<24)|((uint32_t)ram_diag[sbm+1]<<16)|
                                  ((uint32_t)ram_diag[sbm+2]<<8)|ram_diag[sbm+3];
                    uint32_t w1 = ((uint32_t)ram_diag[sbm+4]<<24)|((uint32_t)ram_diag[sbm+5]<<16)|
                                  ((uint32_t)ram_diag[sbm+6]<<8)|ram_diag[sbm+7];
                    uint32_t w2 = ((uint32_t)ram_diag[sbm+80]<<24)|((uint32_t)ram_diag[sbm+81]<<16)|
                                  ((uint32_t)ram_diag[sbm+82]<<8)|ram_diag[sbm+83];
                    /* Also check menu bar area (line 0) and center screen
                     * (line 171 = 342/2, col 28 = near center of 64 byte line) */
                    uint32_t center = sbm + 171 * 64 + 28; /* center of screen */
                    uint32_t menu_mid = sbm + 24;  /* line 0, col 24 (middle area) */
                    uint32_t cw = 0, mw = 0;
                    if (center + 3 < mac_mem_get_ram_size())
                        cw = ((uint32_t)ram_diag[center]<<24)|((uint32_t)ram_diag[center+1]<<16)|
                             ((uint32_t)ram_diag[center+2]<<8)|ram_diag[center+3];
                    if (menu_mid + 3 < mac_mem_get_ram_size())
                        mw = ((uint32_t)ram_diag[menu_mid]<<24)|((uint32_t)ram_diag[menu_mid+1]<<16)|
                             ((uint32_t)ram_diag[menu_mid+2]<<8)|ram_diag[menu_mid+3];
                    ESP_LOGW(TAG, "  FB@$%06X: TL=%08lX +80=%08lX mid=%08lX ctr=%08lX",
                             sb, (unsigned long)w0, (unsigned long)w2,
                             (unsigned long)mw, (unsigned long)cw);
                }
                /* For later frames, dump Finder launch state */
                if (frame_count >= 3000) {
                    uint32_t wmgr = ((uint32_t)ram_diag[0x9DE]<<24)|
                                    ((uint32_t)ram_diag[0x9DF]<<16)|
                                    ((uint32_t)ram_diag[0x9E0]<<8)|ram_diag[0x9E1];
                    uint32_t gray = ((uint32_t)ram_diag[0xA1C]<<24)|
                                    ((uint32_t)ram_diag[0xA1D]<<16)|
                                    ((uint32_t)ram_diag[0xA1E]<<8)|ram_diag[0xA1F];
                    int anl = ram_diag[0x910];
                    char an[32] = {0};
                    if (anl > 0 && anl < 32)
                        for (int i = 0; i < anl; i++) an[i] = ram_diag[0x911+i];
                    uint16_t bd = (ram_diag[0x210]<<8)|ram_diag[0x211];
                    /* $0D94: flag checked by VBlank handler at $C0C0.
                     * $0D30: CurrentA5 (process manager).
                     * $0D2C: CurApRefNum (file ref of current app). */
                    uint16_t flag_0d94 = (ram_diag[0xD94]<<8)|ram_diag[0xD95];
                    uint16_t flag_0d96 = (ram_diag[0xD96]<<8)|ram_diag[0xD97];
                    ESP_LOGW(TAG, "  App=\"%s\" WMgr=$%08lX Gray=$%08lX BD=%d $0D94=%04X $0D96=%04X",
                             an, (unsigned long)wmgr, (unsigned long)gray, bd,
                             flag_0d94, flag_0d96);
                    /* Dump code at current PC for every diagnostic frame */
                    uint32_t rsz2 = mac_mem_get_ram_size();
                    if (diag_pc < rsz2 && diag_pc + 15 < rsz2) {
                        ESP_LOGW(TAG, "  CODE@PC=$%06X: %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X",
                                 diag_pc,
                                 ram_diag[diag_pc],ram_diag[diag_pc+1],
                                 ram_diag[diag_pc+2],ram_diag[diag_pc+3],
                                 ram_diag[diag_pc+4],ram_diag[diag_pc+5],
                                 ram_diag[diag_pc+6],ram_diag[diag_pc+7],
                                 ram_diag[diag_pc+8],ram_diag[diag_pc+9],
                                 ram_diag[diag_pc+10],ram_diag[diag_pc+11],
                                 ram_diag[diag_pc+12],ram_diag[diag_pc+13],
                                 ram_diag[diag_pc+14],ram_diag[diag_pc+15]);
                    }
                }
                /* One-shot: at F3000, dump idle loop code and key traps */
                if (frame_count == 3000) {
                    uint32_t rsz = mac_mem_get_ram_size();

                    /* ---- Zone header dumps ---- */
                    {
                        /* SysZone header at $02A6 -> address stored there */
                        uint32_t sz_addr = ((uint32_t)ram_diag[0x2A6]<<24)|
                                           ((uint32_t)ram_diag[0x2A7]<<16)|
                                           ((uint32_t)ram_diag[0x2A8]<<8)|ram_diag[0x2A9];
                        uint32_t az_addr = ((uint32_t)ram_diag[0x2AA]<<24)|
                                           ((uint32_t)ram_diag[0x2AB]<<16)|
                                           ((uint32_t)ram_diag[0x2AC]<<8)|ram_diag[0x2AD];
                        uint32_t sz = sz_addr & 0xFFFFFF;
                        uint32_t az = az_addr & 0xFFFFFF;
                        ESP_LOGW(TAG, "  SysZone=$%06X ApplZone=$%06X", sz, az);

                        uint32_t td_h = ((uint32_t)ram_diag[0xCA6]<<24)|
                                        ((uint32_t)ram_diag[0xCA7]<<16)|
                                        ((uint32_t)ram_diag[0xCA8]<<8)|ram_diag[0xCA9];
                        uint32_t td_g = td_h & 0xFFFFFF;
                        if (td_g + 0x14 < rsz) {
                            uint32_t td_p = (((uint32_t)ram_diag[td_g+4]<<24)|
                                             ((uint32_t)ram_diag[td_g+5]<<16)|
                                             ((uint32_t)ram_diag[td_g+6]<<8)|ram_diag[td_g+7]) & 0xFFFFFF;
                            uint16_t td_bmhb = (ram_diag[td_g+8]<<8)|ram_diag[td_g+9];
                            uint16_t td_bm_vb = (ram_diag[td_g+0xA]<<8)|ram_diag[td_g+0xB];
                            uint16_t td_cm = (ram_diag[td_g+0x10]<<8)|ram_diag[td_g+0x11];
                            uint32_t td_base = 0;
                            uint16_t td_rb = 0;
                            if (td_p + 0xE < rsz) {
                                td_base = (((uint32_t)ram_diag[td_p+8]<<24)|
                                           ((uint32_t)ram_diag[td_p+9]<<16)|
                                           ((uint32_t)ram_diag[td_p+0xA]<<8)|ram_diag[td_p+0xB]) & 0xFFFFFF;
                                td_rb = (ram_diag[td_p+0xC]<<8)|ram_diag[td_p+0xD];
                            }
                            ESP_LOGW(TAG, "  TheDisplay hdl=$%06X gdev=$%06X port=$%06X base=$%06X rb=%u bmH=%u bmV=%u cm=%d",
                                     (unsigned long)td_h, (unsigned long)td_g, (unsigned long)td_p,
                                     (unsigned long)td_base, (unsigned)td_rb,
                                     (unsigned)td_bmhb, (unsigned)td_bm_vb, (int)td_cm);
                        }

                        /* Dump SysZone header: bkLim(+0), zcbFree(+12), gzProc(+16) */
                        if (sz + 52 < rsz) {
                            uint32_t sz_bklim = ((uint32_t)ram_diag[sz]<<24)|
                                                ((uint32_t)ram_diag[sz+1]<<16)|
                                                ((uint32_t)ram_diag[sz+2]<<8)|ram_diag[sz+3];
                            uint32_t sz_free  = ((uint32_t)ram_diag[sz+12]<<24)|
                                                ((uint32_t)ram_diag[sz+13]<<16)|
                                                ((uint32_t)ram_diag[sz+14]<<8)|ram_diag[sz+15];
                            uint32_t sz_gzp   = ((uint32_t)ram_diag[sz+16]<<24)|
                                                ((uint32_t)ram_diag[sz+17]<<16)|
                                                ((uint32_t)ram_diag[sz+18]<<8)|ram_diag[sz+19];
                            uint32_t sz_alloc = ((uint32_t)ram_diag[sz+46]<<24)|
                                                ((uint32_t)ram_diag[sz+47]<<16)|
                                                ((uint32_t)ram_diag[sz+48]<<8)|ram_diag[sz+49];
                            ESP_LOGW(TAG, "  SysZone HDR: bkLim=$%08lX zcbFree=$%08lX gzProc=$%08lX allocPtr=$%08lX",
                                     (unsigned long)sz_bklim, (unsigned long)sz_free,
                                     (unsigned long)sz_gzp, (unsigned long)sz_alloc);
                        }
                        /* Dump ApplZone header */
                        if (az + 52 < rsz) {
                            uint32_t az_bklim = ((uint32_t)ram_diag[az]<<24)|
                                                ((uint32_t)ram_diag[az+1]<<16)|
                                                ((uint32_t)ram_diag[az+2]<<8)|ram_diag[az+3];
                            uint32_t az_free  = ((uint32_t)ram_diag[az+12]<<24)|
                                                ((uint32_t)ram_diag[az+13]<<16)|
                                                ((uint32_t)ram_diag[az+14]<<8)|ram_diag[az+15];
                            uint32_t az_gzp   = ((uint32_t)ram_diag[az+16]<<24)|
                                                ((uint32_t)ram_diag[az+17]<<16)|
                                                ((uint32_t)ram_diag[az+18]<<8)|ram_diag[az+19];
                            uint32_t az_alloc = ((uint32_t)ram_diag[az+46]<<24)|
                                                ((uint32_t)ram_diag[az+47]<<16)|
                                                ((uint32_t)ram_diag[az+48]<<8)|ram_diag[az+49];
                            ESP_LOGW(TAG, "  ApplZone HDR: bkLim=$%08lX zcbFree=$%08lX gzProc=$%08lX allocPtr=$%08lX",
                                     (unsigned long)az_bklim, (unsigned long)az_free,
                                     (unsigned long)az_gzp, (unsigned long)az_alloc);
                        }
                        /* TheZone, DefltStack, ApplLimit */
                        uint32_t thezone = ((uint32_t)ram_diag[0x118]<<24)|
                                           ((uint32_t)ram_diag[0x119]<<16)|
                                           ((uint32_t)ram_diag[0x11A]<<8)|ram_diag[0x11B];
                        uint32_t dfltstk = ((uint32_t)ram_diag[0x322]<<24)|
                                           ((uint32_t)ram_diag[0x323]<<16)|
                                           ((uint32_t)ram_diag[0x324]<<8)|ram_diag[0x325];
                        uint32_t appllim = ((uint32_t)ram_diag[0x130]<<24)|
                                           ((uint32_t)ram_diag[0x131]<<16)|
                                           ((uint32_t)ram_diag[0x132]<<8)|ram_diag[0x133];
                        ESP_LOGW(TAG, "  TheZone=$%08lX DefltStack=$%08lX ApplLimit=$%08lX",
                                 (unsigned long)thezone, (unsigned long)dfltstk,
                                 (unsigned long)appllim);
                        /* Key OS traps: _SetApplBase($A057), _SetApplLimit($A02D),
                         * _MaxApplZone($A063), _SetZone($A01B) */
                        uint32_t sab_a = 0x0400 + 0x57 * 4;
                        uint32_t sal_a = 0x0400 + 0x2D * 4;
                        uint32_t maz_a = 0x0E00 + 0x63 * 4;
                        uint32_t stzone_a = 0x0400 + 0x1B * 4;
                        uint32_t sab = ((uint32_t)ram_diag[sab_a]<<24)|((uint32_t)ram_diag[sab_a+1]<<16)|
                                       ((uint32_t)ram_diag[sab_a+2]<<8)|ram_diag[sab_a+3];
                        uint32_t sal = ((uint32_t)ram_diag[sal_a]<<24)|((uint32_t)ram_diag[sal_a+1]<<16)|
                                       ((uint32_t)ram_diag[sal_a+2]<<8)|ram_diag[sal_a+3];
                        uint32_t maz = ((uint32_t)ram_diag[maz_a]<<24)|((uint32_t)ram_diag[maz_a+1]<<16)|
                                       ((uint32_t)ram_diag[maz_a+2]<<8)|ram_diag[maz_a+3];
                        uint32_t stz = ((uint32_t)ram_diag[stzone_a]<<24)|((uint32_t)ram_diag[stzone_a+1]<<16)|
                                       ((uint32_t)ram_diag[stzone_a+2]<<8)|ram_diag[stzone_a+3];
                        ESP_LOGW(TAG, "  _SetApplBase=$%06lX _SetApplLimit=$%06lX _MaxApplZone=$%06lX _SetZone=$%06lX",
                                 (unsigned long)(sab & 0xFFFFFF), (unsigned long)(sal & 0xFFFFFF),
                                 (unsigned long)(maz & 0xFFFFFF), (unsigned long)(stz & 0xFFFFFF));
                        /* Count patched TB entries (RAM addresses = below $400000) */
                        int sys_tb = 0, rom_tb = 0;
                        for (int i = 0; i < 0x200; i++) {
                            uint32_t ta = 0x0E00 + i * 4;
                            uint32_t tv = ((uint32_t)ram_diag[ta]<<24)|((uint32_t)ram_diag[ta+1]<<16)|
                                          ((uint32_t)ram_diag[ta+2]<<8)|ram_diag[ta+3];
                            if ((tv & 0x00400000) == 0) sys_tb++; else rom_tb++;
                        }
                        int sys_os = 0, rom_os = 0;
                        for (int i = 0; i < 0x100; i++) {
                            uint32_t ta = 0x0400 + i * 4;
                            uint32_t tv = ((uint32_t)ram_diag[ta]<<24)|((uint32_t)ram_diag[ta+1]<<16)|
                                          ((uint32_t)ram_diag[ta+2]<<8)|ram_diag[ta+3];
                            if ((tv & 0x00400000) == 0) sys_os++; else rom_os++;
                        }
                        ESP_LOGW(TAG, "  TB: %d SYS / %d ROM.  OS: %d SYS / %d ROM",
                                 sys_tb, rom_tb, sys_os, rom_os);
                        /* Print which specific TB entries are SYS-patched */
                        {
                            char tb_buf[128] = {0};
                            int tb_pos = 0;
                            for (int i = 0; i < 0x200 && tb_pos < 120; i++) {
                                uint32_t ta = 0x0E00 + i * 4;
                                uint32_t tv = ((uint32_t)ram_diag[ta]<<24)|((uint32_t)ram_diag[ta+1]<<16)|
                                              ((uint32_t)ram_diag[ta+2]<<8)|ram_diag[ta+3];
                                if ((tv & 0x00400000) == 0)
                                    tb_pos += snprintf(tb_buf + tb_pos, 128 - tb_pos, "%03X ", i);
                            }
                            ESP_LOGW(TAG, "  TB-SYS: %s", tb_buf);
                        }
                        {
                            char os_buf[256] = {0};
                            int os_pos = 0;
                            for (int i = 0; i < 0x100 && os_pos < 250; i++) {
                                uint32_t ta = 0x0400 + i * 4;
                                uint32_t tv = ((uint32_t)ram_diag[ta]<<24)|((uint32_t)ram_diag[ta+1]<<16)|
                                              ((uint32_t)ram_diag[ta+2]<<8)|ram_diag[ta+3];
                                if ((tv & 0x00400000) == 0)
                                    os_pos += snprintf(os_buf + os_pos, 256 - os_pos, "%02X ", i);
                            }
                            ESP_LOGW(TAG, "  OS-SYS: %s", os_buf);
                        }
                    }

                    /* Dump boot code that resets ApplZone to SysZone */
                    if (0x0FF9F0 + 31 < rsz) {
                        ESP_LOGW(TAG, "  CODE@$0FF9F0: %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X",
                                 ram_diag[0xFF9F0],ram_diag[0xFF9F1],ram_diag[0xFF9F2],ram_diag[0xFF9F3],
                                 ram_diag[0xFF9F4],ram_diag[0xFF9F5],ram_diag[0xFF9F6],ram_diag[0xFF9F7],
                                 ram_diag[0xFF9F8],ram_diag[0xFF9F9],ram_diag[0xFF9FA],ram_diag[0xFF9FB],
                                 ram_diag[0xFF9FC],ram_diag[0xFF9FD],ram_diag[0xFF9FE],ram_diag[0xFF9FF]);
                        ESP_LOGW(TAG, "        +16: %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X",
                                 ram_diag[0xFFA00],ram_diag[0xFFA01],ram_diag[0xFFA02],ram_diag[0xFFA03],
                                 ram_diag[0xFFA04],ram_diag[0xFFA05],ram_diag[0xFFA06],ram_diag[0xFFA07],
                                 ram_diag[0xFFA08],ram_diag[0xFFA09],ram_diag[0xFFA0A],ram_diag[0xFFA0B],
                                 ram_diag[0xFFA0C],ram_diag[0xFFA0D],ram_diag[0xFFA0E],ram_diag[0xFFA0F]);
                    }
                    /* Raw zone header dump at $1600 (52 bytes) */
                    if (0x1600 + 51 < rsz) {
                        ESP_LOGW(TAG, "  ZONE@$1600: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                                 ram_diag[0x1600],ram_diag[0x1601],ram_diag[0x1602],ram_diag[0x1603],
                                 ram_diag[0x1604],ram_diag[0x1605],ram_diag[0x1606],ram_diag[0x1607],
                                 ram_diag[0x1608],ram_diag[0x1609],ram_diag[0x160A],ram_diag[0x160B],
                                 ram_diag[0x160C],ram_diag[0x160D],ram_diag[0x160E],ram_diag[0x160F]);
                        ESP_LOGW(TAG, "        +16: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                                 ram_diag[0x1610],ram_diag[0x1611],ram_diag[0x1612],ram_diag[0x1613],
                                 ram_diag[0x1614],ram_diag[0x1615],ram_diag[0x1616],ram_diag[0x1617],
                                 ram_diag[0x1618],ram_diag[0x1619],ram_diag[0x161A],ram_diag[0x161B],
                                 ram_diag[0x161C],ram_diag[0x161D],ram_diag[0x161E],ram_diag[0x161F]);
                        ESP_LOGW(TAG, "        +32: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                                 ram_diag[0x1620],ram_diag[0x1621],ram_diag[0x1622],ram_diag[0x1623],
                                 ram_diag[0x1624],ram_diag[0x1625],ram_diag[0x1626],ram_diag[0x1627],
                                 ram_diag[0x1628],ram_diag[0x1629],ram_diag[0x162A],ram_diag[0x162B],
                                 ram_diag[0x162C],ram_diag[0x162D],ram_diag[0x162E],ram_diag[0x162F]);
                        ESP_LOGW(TAG, "        +48: %02X%02X%02X%02X",
                                 ram_diag[0x1630],ram_diag[0x1631],ram_diag[0x1632],ram_diag[0x1633]);
                    }

                    /* Dump 16 words at $00C0C0 (idle loop code in RAM) */
                    if (0xC0C0 + 31 < rsz) {
                        ESP_LOGW(TAG, "  CODE@$C0C0: %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X",
                                 ram_diag[0xC0C0],ram_diag[0xC0C1],ram_diag[0xC0C2],ram_diag[0xC0C3],
                                 ram_diag[0xC0C4],ram_diag[0xC0C5],ram_diag[0xC0C6],ram_diag[0xC0C7],
                                 ram_diag[0xC0C8],ram_diag[0xC0C9],ram_diag[0xC0CA],ram_diag[0xC0CB],
                                 ram_diag[0xC0CC],ram_diag[0xC0CD],ram_diag[0xC0CE],ram_diag[0xC0CF]);
                        ESP_LOGW(TAG, "         +16: %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X",
                                 ram_diag[0xC0D0],ram_diag[0xC0D1],ram_diag[0xC0D2],ram_diag[0xC0D3],
                                 ram_diag[0xC0D4],ram_diag[0xC0D5],ram_diag[0xC0D6],ram_diag[0xC0D7],
                                 ram_diag[0xC0D8],ram_diag[0xC0D9],ram_diag[0xC0DA],ram_diag[0xC0DB],
                                 ram_diag[0xC0DC],ram_diag[0xC0DD],ram_diag[0xC0DE],ram_diag[0xC0DF]);
                    }
                    /* Dump _OSDispatch ($A88F = TB[$8F]) — System 7.5 uses this
                     * for LaunchApplication instead of old _Launch ($A9F2).
                     * $A88F: bit 11=1 → Toolbox trap, index=$8F */
                    uint32_t osd_a = 0x0E00 + 0x8F * 4;  /* TB trap table */
                    uint32_t osd = ((uint32_t)ram_diag[osd_a]<<24)|
                                   ((uint32_t)ram_diag[osd_a+1]<<16)|
                                   ((uint32_t)ram_diag[osd_a+2]<<8)|ram_diag[osd_a+3];
                    /* _WaitNextEvent ($A860 = TB[$060]) */
                    uint32_t wne_a = 0x0E00 + 0x060 * 4;  /* TB trap table */
                    uint32_t wne = ((uint32_t)ram_diag[wne_a]<<24)|
                                   ((uint32_t)ram_diag[wne_a+1]<<16)|
                                   ((uint32_t)ram_diag[wne_a+2]<<8)|ram_diag[wne_a+3];
                    /* _GetNextEvent ($A970 = TB[$170]) */
                    uint32_t gne_a = 0x0E00 + 0x170 * 4;
                    uint32_t gne = ((uint32_t)ram_diag[gne_a]<<24)|
                                   ((uint32_t)ram_diag[gne_a+1]<<16)|
                                   ((uint32_t)ram_diag[gne_a+2]<<8)|ram_diag[gne_a+3];
                    /* _InitProcess / _LaunchDeskAccessory area */
                    uint32_t lda_a = 0x0E00 + 0x1F2 * 4;  /* _Launch */
                    uint32_t lda = ((uint32_t)ram_diag[lda_a]<<24)|
                                   ((uint32_t)ram_diag[lda_a+1]<<16)|
                                   ((uint32_t)ram_diag[lda_a+2]<<8)|ram_diag[lda_a+3];
                    ESP_LOGW(TAG, "  _OSDispatch=$%06lX _WaitNextEvt=$%06lX _GetNextEvt=$%06lX _Launch=$%06lX",
                             (unsigned long)(osd & 0xFFFFFF), (unsigned long)(wne & 0xFFFFFF),
                             (unsigned long)(gne & 0xFFFFFF), (unsigned long)(lda & 0xFFFFFF));
                    /* Stack dump at idle — shows call chain */
                    uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF;
                    uint32_t spm = sp & (rsz - 1);
                    if (spm + 63 < rsz) {
                        ESP_LOGW(TAG, "  STACK@$%06X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                                 sp,
                                 ram_diag[spm],ram_diag[spm+1],ram_diag[spm+2],ram_diag[spm+3],
                                 ram_diag[spm+4],ram_diag[spm+5],ram_diag[spm+6],ram_diag[spm+7],
                                 ram_diag[spm+8],ram_diag[spm+9],ram_diag[spm+10],ram_diag[spm+11],
                                 ram_diag[spm+12],ram_diag[spm+13],ram_diag[spm+14],ram_diag[spm+15]);
                        ESP_LOGW(TAG, "         +16: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                                 ram_diag[spm+16],ram_diag[spm+17],ram_diag[spm+18],ram_diag[spm+19],
                                 ram_diag[spm+20],ram_diag[spm+21],ram_diag[spm+22],ram_diag[spm+23],
                                 ram_diag[spm+24],ram_diag[spm+25],ram_diag[spm+26],ram_diag[spm+27],
                                 ram_diag[spm+28],ram_diag[spm+29],ram_diag[spm+30],ram_diag[spm+31]);
                        ESP_LOGW(TAG, "         +32: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                                 ram_diag[spm+32],ram_diag[spm+33],ram_diag[spm+34],ram_diag[spm+35],
                                 ram_diag[spm+36],ram_diag[spm+37],ram_diag[spm+38],ram_diag[spm+39],
                                 ram_diag[spm+40],ram_diag[spm+41],ram_diag[spm+42],ram_diag[spm+43],
                                 ram_diag[spm+44],ram_diag[spm+45],ram_diag[spm+46],ram_diag[spm+47]);
                    }
                    /* Dump CurrentA5 ($0904), CurStackBase ($0908) */
                    uint32_t curA5 = ((uint32_t)ram_diag[0x904]<<24)|((uint32_t)ram_diag[0x905]<<16)|
                                     ((uint32_t)ram_diag[0x906]<<8)|ram_diag[0x907];
                    uint32_t curSB = ((uint32_t)ram_diag[0x908]<<24)|((uint32_t)ram_diag[0x909]<<16)|
                                     ((uint32_t)ram_diag[0x90A]<<8)|ram_diag[0x90B];
                    int16_t curRef = (int16_t)((ram_diag[0x900]<<8)|ram_diag[0x901]);
                    ESP_LOGW(TAG, "  CurApRefNum=%d CurrentA5=$%08lX CurStackBase=$%08lX",
                             curRef, (unsigned long)curA5, (unsigned long)curSB);
                    /* Dump 32 bytes of code at current PC to identify the stable loop */
                    uint32_t loop_pc = diag_pc;
                    if (loop_pc < rsz && loop_pc + 31 < rsz) {
                        ESP_LOGW(TAG, "  CODE@PC=$%06X: %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X",
                                 loop_pc,
                                 ram_diag[loop_pc],ram_diag[loop_pc+1],
                                 ram_diag[loop_pc+2],ram_diag[loop_pc+3],
                                 ram_diag[loop_pc+4],ram_diag[loop_pc+5],
                                 ram_diag[loop_pc+6],ram_diag[loop_pc+7],
                                 ram_diag[loop_pc+8],ram_diag[loop_pc+9],
                                 ram_diag[loop_pc+10],ram_diag[loop_pc+11],
                                 ram_diag[loop_pc+12],ram_diag[loop_pc+13],
                                 ram_diag[loop_pc+14],ram_diag[loop_pc+15]);
                        ESP_LOGW(TAG, "           +16: %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X",
                                 ram_diag[loop_pc+16],ram_diag[loop_pc+17],
                                 ram_diag[loop_pc+18],ram_diag[loop_pc+19],
                                 ram_diag[loop_pc+20],ram_diag[loop_pc+21],
                                 ram_diag[loop_pc+22],ram_diag[loop_pc+23],
                                 ram_diag[loop_pc+24],ram_diag[loop_pc+25],
                                 ram_diag[loop_pc+26],ram_diag[loop_pc+27],
                                 ram_diag[loop_pc+28],ram_diag[loop_pc+29],
                                 ram_diag[loop_pc+30],ram_diag[loop_pc+31]);
                    } else {
                        ESP_LOGW(TAG, "  CODE@PC=$%06X (ROM or out of RAM range)", loop_pc);
                    }

                    /* Dump resource trap log from trampoline buffer at $1FA000 */
                    if (0x1FA500 < rsz) {
                        uint32_t next_ptr = ((uint32_t)ram_diag[0x1FA000]<<24)|
                                            ((uint32_t)ram_diag[0x1FA001]<<16)|
                                            ((uint32_t)ram_diag[0x1FA002]<<8)|ram_diag[0x1FA003];
                        int num_entries = (int)((next_ptr - 0x1FA008) / 8);
                        if (num_entries < 0) num_entries = 0;
                        if (num_entries > 156) num_entries = 156;
                        ESP_LOGW(TAG, "  RES-LOG: %d entries (next=$%06X)",
                                 num_entries, (unsigned)(next_ptr & 0xFFFFFF));
                        for (int ei = 0; ei < num_entries && ei < 100; ei++) {
                            uint32_t ea = 0x1FA008 + ei * 8;
                            uint16_t tw = (ram_diag[ea]<<8)|ram_diag[ea+1];
                            char rtype[5];
                            rtype[0] = ram_diag[ea+2]; rtype[1] = ram_diag[ea+3];
                            rtype[2] = ram_diag[ea+4]; rtype[3] = ram_diag[ea+5];
                            rtype[4] = '\0';
                            for (int ci = 0; ci < 4; ci++)
                                if (rtype[ci] < 0x20 || rtype[ci] > 0x7E) rtype[ci] = '?';
                            int16_t rid = (int16_t)((ram_diag[ea+6]<<8)|ram_diag[ea+7]);
                            const char *tname =
                                (tw == 0xA9A0) ? "GetRes" :
                                (tw == 0xA9A7) ? "Get1Res" :
                                (tw == 0xA99B) ? "CntRes" :
                                (tw == 0xA99C) ? "GetIndRes" : "???";
                            ESP_LOGW(TAG, "    RL[%d] %s('%s',%d)",
                                     ei, tname, rtype, rid);
                        }
                    }

                    /* Dump code at $8E00-$9200 — stall loop.
                     * Full dump with stride=32 for complete disassembly. */
                    if (0x9200 < rsz) {
                        ESP_LOGW(TAG, "  STALL-CODE@$8E00:");
                        for (uint32_t sa = 0x8E00; sa < 0x9200; sa += 32) {
                            ESP_LOGW(TAG, "    $%04X: %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X"
                                     " %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X",
                                     sa,
                                     ram_diag[sa],ram_diag[sa+1],ram_diag[sa+2],ram_diag[sa+3],
                                     ram_diag[sa+4],ram_diag[sa+5],ram_diag[sa+6],ram_diag[sa+7],
                                     ram_diag[sa+8],ram_diag[sa+9],ram_diag[sa+10],ram_diag[sa+11],
                                     ram_diag[sa+12],ram_diag[sa+13],ram_diag[sa+14],ram_diag[sa+15],
                                     ram_diag[sa+16],ram_diag[sa+17],ram_diag[sa+18],ram_diag[sa+19],
                                     ram_diag[sa+20],ram_diag[sa+21],ram_diag[sa+22],ram_diag[sa+23],
                                     ram_diag[sa+24],ram_diag[sa+25],ram_diag[sa+26],ram_diag[sa+27],
                                     ram_diag[sa+28],ram_diag[sa+29],ram_diag[sa+30],ram_diag[sa+31]);
                        }
                    }

                    /* Also dump code at $B300-$B400 — _InitWindows handler
                     * (patched to $00B322 by System file) */
                    if (0xB400 < rsz) {
                        ESP_LOGW(TAG, "  INITWIN-CODE@$B300:");
                        for (uint32_t sa = 0xB300; sa < 0xB400; sa += 32) {
                            ESP_LOGW(TAG, "    $%04X: %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X"
                                     " %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X",
                                     sa,
                                     ram_diag[sa],ram_diag[sa+1],ram_diag[sa+2],ram_diag[sa+3],
                                     ram_diag[sa+4],ram_diag[sa+5],ram_diag[sa+6],ram_diag[sa+7],
                                     ram_diag[sa+8],ram_diag[sa+9],ram_diag[sa+10],ram_diag[sa+11],
                                     ram_diag[sa+12],ram_diag[sa+13],ram_diag[sa+14],ram_diag[sa+15],
                                     ram_diag[sa+16],ram_diag[sa+17],ram_diag[sa+18],ram_diag[sa+19],
                                     ram_diag[sa+20],ram_diag[sa+21],ram_diag[sa+22],ram_diag[sa+23],
                                     ram_diag[sa+24],ram_diag[sa+25],ram_diag[sa+26],ram_diag[sa+27],
                                     ram_diag[sa+28],ram_diag[sa+29],ram_diag[sa+30],ram_diag[sa+31]);
                        }
                    }

                    /* Dump code at $A900-$AB00 — the subroutine called by stall loop */
                    if (0xAB00 < rsz) {
                        ESP_LOGW(TAG, "  CALLEE-CODE@$A900:");
                        for (uint32_t sa = 0xA900; sa < 0xAB00; sa += 32) {
                            ESP_LOGW(TAG, "    $%04X: %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X"
                                     " %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X",
                                     sa,
                                     ram_diag[sa],ram_diag[sa+1],ram_diag[sa+2],ram_diag[sa+3],
                                     ram_diag[sa+4],ram_diag[sa+5],ram_diag[sa+6],ram_diag[sa+7],
                                     ram_diag[sa+8],ram_diag[sa+9],ram_diag[sa+10],ram_diag[sa+11],
                                     ram_diag[sa+12],ram_diag[sa+13],ram_diag[sa+14],ram_diag[sa+15],
                                     ram_diag[sa+16],ram_diag[sa+17],ram_diag[sa+18],ram_diag[sa+19],
                                     ram_diag[sa+20],ram_diag[sa+21],ram_diag[sa+22],ram_diag[sa+23],
                                     ram_diag[sa+24],ram_diag[sa+25],ram_diag[sa+26],ram_diag[sa+27],
                                     ram_diag[sa+28],ram_diag[sa+29],ram_diag[sa+30],ram_diag[sa+31]);
                        }
                    }

                    /* Also dump $9200-$9400 — continuation of stall loop dispatch */
                    if (0x9400 < rsz) {
                        ESP_LOGW(TAG, "  STALL2-CODE@$9200:");
                        for (uint32_t sa = 0x9200; sa < 0x9400; sa += 32) {
                            ESP_LOGW(TAG, "    $%04X: %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X"
                                     " %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X",
                                     sa,
                                     ram_diag[sa],ram_diag[sa+1],ram_diag[sa+2],ram_diag[sa+3],
                                     ram_diag[sa+4],ram_diag[sa+5],ram_diag[sa+6],ram_diag[sa+7],
                                     ram_diag[sa+8],ram_diag[sa+9],ram_diag[sa+10],ram_diag[sa+11],
                                     ram_diag[sa+12],ram_diag[sa+13],ram_diag[sa+14],ram_diag[sa+15],
                                     ram_diag[sa+16],ram_diag[sa+17],ram_diag[sa+18],ram_diag[sa+19],
                                     ram_diag[sa+20],ram_diag[sa+21],ram_diag[sa+22],ram_diag[sa+23],
                                     ram_diag[sa+24],ram_diag[sa+25],ram_diag[sa+26],ram_diag[sa+27],
                                     ram_diag[sa+28],ram_diag[sa+29],ram_diag[sa+30],ram_diag[sa+31]);
                        }
                    }

                    /* Dump total trap ring stats */
                    ESP_LOGW(TAG, "  TRAP-RING: %d total traps captured", trap_ring_idx);
                    {
                        int n = trap_ring_idx < 32 ? trap_ring_idx : 32;
                        for (int i = 0; i < n; i++) {
                            int idx = ((trap_ring_idx - n + i) % 32 + 32) % 32;
                            ESP_LOGW(TAG, "    T[%d] $%04X from $%06X",
                                     i, trap_ring[idx],
                                     trap_caller_ring[idx] & 0xFFFFFF);
                        }
                    }
                }
            }
            /* At F900, if $0172 is still non-negative and _Read hasn't
             * happened, the VBL task failed to detect disk insertion.
             * Force $0172 negative to unblock the boot _Read gate. */
            if (frame_count == 900 && ram_diag && !(ram_diag[0x172] & 0x80)) {
                /* Also need the event to be in the queue (sub3 checks it).
                 * Check $030A (event queue pointer): if 0, no events posted.
                 * We'll force $0172=0xFF AND try to help sub3 find an event. */
                uint32_t evtq = (ram_diag) ?
                    ((uint32_t)ram_diag[0x30A]<<24)|((uint32_t)ram_diag[0x30B]<<16)|
                    ((uint32_t)ram_diag[0x30C]<<8)|ram_diag[0x30D] : 0;
                ESP_LOGW(TAG, "FORCE: $172=FF (was %02X) evtQ=%08lX",
                         ram_diag[0x172], (unsigned long)evtq);
                ram_diag[0x172] = 0xFF;
            }
        }

        /* Detect first entry into ROM idle loop ($402BE2 = STOP #$2000).
         * This tells us when the system settles after boot/INITs. */
        {
            static bool idle_loop_logged = false;
            static int idle_loop_frames = 0;
            uint32_t pc_idle = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            if (pc_idle >= 0x402BE0 && pc_idle <= 0x402BF8) {
                idle_loop_frames++;
                if (!idle_loop_logged && idle_loop_frames >= 3) {
                    idle_loop_logged = true;
                    /* Enable SCSI register emulation now that Finder is stable.
                     * Must NOT be enabled during boot/INIT loading — SCSI Manager
                     * init changes heap layout, causing crash at F5239. */
                    scsi_set_active(true);
                    ESP_LOGW(TAG, "IDLE-LOOP entered at F%d PC=$%06X SP=$%06X",
                             frame_count, pc_idle,
                             m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
                    /* Dump critical low-mem globals at idle entry */
                    uint8_t *rr = mac_mem_get_ram();
                    if (rr) {
                        /* CurApName at $0910 (32 bytes, pascal string) */
                        int nl = rr[0x910];
                        char appname[32] = {0};
                        if (nl > 0 && nl < 32) {
                            for (int i = 0; i < nl; i++) appname[i] = rr[0x911+i];
                        }
                        /* FinderName at $02E0 (16 bytes, pascal string) */
                        int fnl = rr[0x2E0];
                        char fname[16] = {0};
                        if (fnl > 0 && fnl < 16) {
                            for (int i = 0; i < fnl; i++) fname[i] = rr[0x2E1+i];
                        }
                        uint32_t cur_ap = ((uint32_t)rr[0x900]<<24)|((uint32_t)rr[0x901]<<16)|
                                          ((uint32_t)rr[0x902]<<8)|rr[0x903];
                        ESP_LOGW(TAG, "  CurApName=\"%s\" FinderName=\"%s\" CurApRefNum=$%08lX",
                                 appname, fname, (unsigned long)cur_ap);
                        /* BootDrive at $0210 */
                        uint16_t boot_drv = (rr[0x210]<<8)|rr[0x211];
                        /* SysMap at $0A58 (refNum of System resource file) */
                        int16_t sys_map = (int16_t)((rr[0xA58]<<8)|rr[0xA59]);
                        ESP_LOGW(TAG, "  BootDrive=%d SysMap=%d",
                                 boot_drv, sys_map);
                    }
                }
            } else {
                idle_loop_frames = 0;
            }
        }

        /* ADB polling bypass: runs every frame.
         * The ADB INIT at $08444A polls a completion flag byte at (A0).
         * Without ADB microcontroller emulation, the flag never gets set.
         * Detect the stall and write 0xFF to simulate "no device". */
        {
            uint32_t adb_pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            static int adb_stall_frames = 0;
            static bool adb_logged = false;
            if (adb_pc >= 0x08443A && adb_pc <= 0x08444C) {
                adb_stall_frames++;
                if (adb_stall_frames >= 2) {
                    uint8_t *adb_ram = mac_mem_get_ram();
                    uint32_t adb_rsz = mac_mem_get_ram_size();
                    uint32_t adb_a0 = m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF;
                    if (adb_ram && adb_a0 < adb_rsz) {
                        adb_ram[adb_a0] = 0xFF;
                        if (!adb_logged) {
                            adb_logged = true;
                            ESP_LOGW(TAG, "ADB-BYPASS: writing $FF to $%06X every frame F%d",
                                     adb_a0, frame_count);
                        }
                    }
                }
            } else {
                if (adb_stall_frames >= 2) adb_stall_frames = 0;
            }
        }

        /* Frame log: sparse to minimize serial overhead.
         * During disk reads, log more frequently to diagnose stuck states.
         * Also log every 50 frames between F700-F2000 to diagnose stalls. */
        bool disk_log = iwm_is_reading() && (frame_count % 30 == 0);
        bool stall_diag = (frame_count >= 700 && frame_count <= 2000 &&
                           frame_count % 50 == 0);
        if (frame_count < 3 ||
            disk_log || stall_diag ||
            (frame_count % 300 == 0 && frame_count <= 6000) ||
            (frame_count % 1000 == 0 && frame_count <= 30000)) {
            uint8_t *ram_fl = mac_mem_get_ram();
            uint8_t v0172 = (ram_fl) ? ram_fl[0x172] : 0xFF;
            ESP_LOGI(TAG, "F%d: PC=%06X SP=%06X SR=%04X D0=%08X A0=%06X $172=%02X spoll=%d",
                     frame_count,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF,
                     m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF,
                     m68k_get_reg(NULL, M68K_REG_SR),
                     m68k_get_reg(NULL, M68K_REG_D0),
                     m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF,
                     v0172,
                     iwm_get_sense_poll_count());

            /* (ADB bypass moved to run every frame — see above) */


            /* If PC is in the decompressor ($8E00-$AB00), dump bitstream progress */
            uint32_t fl_pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            if (fl_pc >= 0x8E00 && fl_pc < 0xAB00 && ram_fl &&
                (frame_count % 1000 == 0) && frame_count >= 700) {
                uint32_t a4v = m68k_get_reg(NULL, M68K_REG_A4) & 0xFFFFFF;
                if (a4v > 0 && a4v + 16 < 0x200000) {
                    uint32_t dp = ((uint32_t)ram_fl[a4v]<<24)|((uint32_t)ram_fl[a4v+1]<<16)|
                                  ((uint32_t)ram_fl[a4v+2]<<8)|ram_fl[a4v+3];
                    uint32_t acc = ((uint32_t)ram_fl[a4v+4]<<24)|((uint32_t)ram_fl[a4v+5]<<16)|
                                   ((uint32_t)ram_fl[a4v+6]<<8)|ram_fl[a4v+7];
                    uint32_t ovc = ((uint32_t)ram_fl[a4v+8]<<24)|((uint32_t)ram_fl[a4v+9]<<16)|
                                   ((uint32_t)ram_fl[a4v+10]<<8)|ram_fl[a4v+11];
                    uint32_t br = ((uint32_t)ram_fl[a4v+12]<<24)|((uint32_t)ram_fl[a4v+13]<<16)|
                                  ((uint32_t)ram_fl[a4v+14]<<8)|ram_fl[a4v+15];
                    ESP_LOGW(TAG, "  DCMP F%d: A4=$%06X data_ptr=$%06X accum=$%08X ovfl=%d bits_rem=%d",
                             frame_count, a4v, dp, acc, ovc, br);
                }
            }

            /* Detect when PC leaves decompressor range */
            {
                static bool was_in_dcmp = false;
                bool in_dcmp = (fl_pc >= 0x8E00 && fl_pc < 0xAB00);
                if (was_in_dcmp && !in_dcmp && frame_count > 700) {
                    ESP_LOGW(TAG, "*** DCMP-EXIT F%d: PC=$%06X SP=$%06X ***",
                             frame_count, fl_pc,
                             m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
                }
                if (in_dcmp && !was_in_dcmp && frame_count > 700) {
                    ESP_LOGW(TAG, "*** DCMP-ENTER F%d: PC=$%06X SP=$%06X ***",
                             frame_count, fl_pc,
                             m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
                }
                was_in_dcmp = in_dcmp;
            }
        }

#if !MAC_ROM_LC_II
        /* --- Classic shutdown screen detection ---
         * After Special > Shut Down the ROM draws "It is now safe to switch
         * off your Macintosh." and polls MBState in $401602-$401634 waiting
         * for a click on Restart. Log once so the ESP32 could later blank
         * the backlight (no power control here). */
        {
            static bool sd_logged = false;
            uint32_t sd_pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            if (!sd_logged && mac_post_boot && sd_pc >= 0x401602 && sd_pc < 0x401640) {
                ESP_LOGW(TAG, "SHUTDOWN: safe to power off (F%d)", frame_count);
                disk_ov_log_stats("shutdown");   /* writable disk */
                sd_logged = true;
            }
        }
        /* --- end shutdown detection --- */
#endif

        frame_count++;

#if MAC_ROM_LC_II
        v8disk_frame();   /* .Sony Prime -> embedded image (mac_v8disk.c) */
        {
            uint32_t pc_ring = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            v8_ring[v8_ri & 31] = pc_ring;
            v8_ri++;
            if (pc_ring < 0x800000) v8_page[pc_ring >> 14]++;
            else if (pc_ring >= 0xA00000 && pc_ring < 0xA80000) v8_page[500 + ((pc_ring >> 14) & 7)]++;
            v8_ps_cum++;
            if (mac_post_boot && !v8_cleared) {
                v8_cleared = true;
                v8_ps_cum = 1;
                for (int i = 0; i < 512; i++) v8_page[i] = 0;
                for (int i = 0; i < 16; i++) v8_pv[i] = 0;
            }
        }
        if ((frame_count & 3) == 0) {
#else
        if ((frame_count & 63) == 0) {
#endif
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
#if MAC_ROM_LC_II
            if (pc < 0x800000 || (pc >= 0xA00000 && pc < 0xA80000)) {
                uint32_t key = (pc >> 4) + 1;
                int h = (int)((key * 2654435761u) >> 28) & 15;
                int tries = 0;
                while (tries < 16 && v8_pk[h] && v8_pk[h] != key) { h = (h + 1) & 15; tries++; }
                if (tries == 16) {
                    int min = 0;
                    for (int i = 1; i < 16; i++)
                        if (v8_pv[i] < v8_pv[min]) min = i;
                    v8_pk[min] = key; v8_pv[min] = 1;
                } else if (v8_pk[h]) v8_pv[h]++;
                else { v8_pk[h] = key; v8_pv[h] = 1; }
            } else pc_bad_count++;
#else
            if (pc < 0x200000) pc_hist[pc >> 12]++;
            else if (pc >= 0x400000 && pc < 0x480000) pc_hist[512 + ((pc - 0x400000) >> 12)]++;
            else pc_bad_count++;
#endif
            pc_samples++;
        }

        /* Track DSErrCode ($0AF0) changes and dump state around boot 2/3 */
        {
            static int16_t last_dserr = 0;
            uint8_t *rd = mac_mem_get_ram();
            if (rd) {
                int16_t cur_dserr = (int16_t)((rd[0xAF0]<<8)|rd[0xAF1]);
                if (cur_dserr != last_dserr) {
                    int16_t res_err = (int16_t)((rd[0xA60]<<8)|rd[0xA61]);
                    ESP_LOGE(TAG, "DSERR-CHG: DSErr %d->%d ResErr=%d PC=%06X D0=%08X F%d",
                             last_dserr, cur_dserr, res_err,
                             m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF,
                             m68k_get_reg(NULL, M68K_REG_D0), frame_count);
                    last_dserr = cur_dserr;
                }
                /* Detailed dump at frames 633-642 (boot 2/3 execution window) */
                if (frame_count >= 633 && frame_count <= 642) {
                    int16_t res_err = (int16_t)((rd[0xA60]<<8)|rd[0xA61]);
                    uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
                    uint32_t d0 = m68k_get_reg(NULL, M68K_REG_D0);
                    uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF;
                    uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF;
                    uint32_t a5 = m68k_get_reg(NULL, M68K_REG_A5) & 0xFFFFFF;
                    uint32_t cur_a5_lm = ((uint32_t)rd[0x904]<<24)|((uint32_t)rd[0x905]<<16)|
                                          ((uint32_t)rd[0x906]<<8)|rd[0x907];
                    ESP_LOGW(TAG, "BOOT3-F%d: PC=%06X SP=%06X D0=%08X A5=%06X [$904]=%08lX DSErr=%d ResErr=%d",
                             frame_count, pc, sp, d0, a5, (unsigned long)cur_a5_lm, cur_dserr, res_err);
                }
            }
        }

        /* All patches at F600, disk insert at F650 — see above */

        /* Profiling: accumulate frame timing */
        {
            uint32_t frame_cend = esp_cpu_get_cycle_count();
            prof_total_sum += (frame_cend - frame_cstart);
            prof_frame_count++;
        }

        /* VBlank is now fired mid-frame (inside the execution loop above).
         * If it wasn't fired this frame (shouldn't happen), fire it now. */
        if (!vblank_fired) {
            via_vblank();
#if MAC_ROM_LC_II
            v8video_vbl(true);
#endif
        }

        /* Write mouse state to Mac low-memory globals once per frame.
         * Mac Point format: big-endian, 4 bytes = (Y.hi Y.lo X.hi X.lo).
         * MBState bit 7: 0 = button down, 1 = button up.
         * Gate on WMgrPort ($09DE) being initialized, AND only write when
         * mouse position/button actually changes. Writing CrsrNew=$FF
         * unconditionally during INIT/Finder loading crashes the resource
         * decompressor (DCMP) — cursor redraw interrupts corrupt its state. */
#if !MAC_ROM_LC_II
        /* (V8 builds skip this: v8_apply_touch() writes the position and
         * the button goes through the emulated ADB mouse, whose OS driver
         * posts the events.) */
        if (mac_post_boot) {
            uint8_t *r = mac_mem_get_ram();
            /* Check WMgrPort at $09DE — $FFFFFFFF means Window Manager not yet init'd */
            uint32_t wmgr = r ? ((uint32_t)r[0x9DE]<<24)|((uint32_t)r[0x9DF]<<16)|
                                ((uint32_t)r[0x9E0]<<8)|r[0x9E1] : 0xFFFFFFFF;
            if (r && wmgr != 0xFFFFFFFF && wmgr != 0x00000000) {
                int mx = mouse_x;
                int my = mouse_y;
                bool mb = mouse_button;

                /* Only write mouse globals and trigger cursor redraw when
                 * something actually changed. Writing CrsrNew=$FF every frame
                 * forces cursor redraws that crash the decompressor during
                 * INIT/Finder loading (before cursor structures are ready). */
                static int prev_mx = MAC_SCREEN_WIDTH / 2;
                static int prev_my = MAC_SCREEN_HEIGHT / 2;
                static int prev_mb = 0;
                bool pos_changed = (mx != prev_mx || my != prev_my);
                bool btn_changed = ((int)mb != prev_mb);

                if (pos_changed) {
                    prev_mx = mx;
                    prev_my = my;

                    /* MTemp $0828 — raw mouse position before acceleration */
                    r[0x0828] = (my >> 8) & 0xFF;
                    r[0x0829] = my & 0xFF;
                    r[0x082A] = (mx >> 8) & 0xFF;
                    r[0x082B] = mx & 0xFF;

                    /* RawMouse $082C — raw mouse position */
                    r[0x082C] = (my >> 8) & 0xFF;
                    r[0x082D] = my & 0xFF;
                    r[0x082E] = (mx >> 8) & 0xFF;
                    r[0x082F] = mx & 0xFF;

                    /* Mouse $0830 — current mouse position */
                    r[0x0830] = (my >> 8) & 0xFF;
                    r[0x0831] = my & 0xFF;
                    r[0x0832] = (mx >> 8) & 0xFF;
                    r[0x0833] = mx & 0xFF;

                    /* CrsrNew $08CE — trigger cursor redraw only on movement */
                    r[0x08CE] = 0xFF;

                    /* CrsrCouple $08CF — cursor tracks mouse */
                    r[0x08CF] = 0xFF;
                }

                /* Update VIA PB3 (hardware mouse button, active low) */
                via_set_mouse_button(mb);

                /* MBState $0172 — write every frame (Mac code may overwrite).
                 * bit 7: 0=down, 1=up */
                /* Whole byte, as the ROM writes it ($00/$80): the shutdown
                 * screen's click loop ($401602) waits for exactly $00 */
                r[0x0172] = mb ? 0x00 : 0x80;

                if (btn_changed) {
                    prev_mb = (int)mb;
                    /* Post mouseDown/mouseUp event to the Mac event queue.
                     * This is essential: the Mac Classic event system (GetNextEvent/
                     * WaitNextEvent) reads from the queue, not from MBState.
                     * Without this, clicks are invisible to applications. */
                    static bool gm_click;   /* decided at down, kept for up */
#if TOUCH_GAME_MODE
                    if (mb) gm_click = game_mode_active();
#endif
                    mouse_btn_queue(mb, gm_click);
                    mouse_btn_flush(r, mx, my);
                    static int btn_log = 0;
                    if (btn_log < 30) {
                        ESP_LOGW(TAG, "MOUSE-BTN: %s F%d mx=%d my=%d $172=%02X evQ=%08lX%s%s",
                                 mb ? "DOWN" : "UP", frame_count, mx, my, r[0x0172],
                                 (unsigned long)rd32(r, EVTQ_HEAD),
                                 mb_pend_n ? " (deferred: SR mask)" : "",
                                 gm_click ? " (game mode: posted at menu bar)" : "");
                        btn_log++;
                    }
                } else {
                    mouse_btn_flush(r, mx, my);   /* deferred transitions */
                }
            }
        }
#endif /* !MAC_ROM_LC_II */

        /* One-second interrupt every 60 frames (~1Hz) */
        if (frame_count % 60 == 0) {
            via_one_second();

            /* Log emulation speed every 5 Mac-seconds (300 frames).
             * Compare wall-clock to emulated time for speed ratio. */
            static int64_t speed_base_us = 0;
            static int speed_base_frame = 0;
            if (speed_base_us == 0) {
                speed_base_us = esp_timer_get_time();
                speed_base_frame = frame_count;
            } else if ((frame_count - speed_base_frame) >= 300) {
                int64_t wall_us = esp_timer_get_time() - speed_base_us;
                int frames_done = frame_count - speed_base_frame;
                /* Mac time for these frames: frames / 60.15 seconds */
                int mac_ms = (frames_done * 1000) / 60;
                int wall_ms = (int)(wall_us / 1000);
                int speed_pct = (wall_ms > 0) ? (mac_ms * 100 / wall_ms) : 0;
                ESP_LOGI(TAG, "SPEED: %d frames in %dms (mac=%dms) => %d%% real-time",
                         frames_done, wall_ms, mac_ms, speed_pct);
                ESP_LOGI(TAG, "MACPC: %06X", m68k_get_reg(NULL, M68K_REG_PC));
#if MAC_IDLE_FF
                ESP_LOGI(TAG, "IDLE-FF: %u%% of CPU cycles skipped (%u FFs: %u idle-loop, %u tick-wait)",
                         (unsigned)((uint64_t)ff_stat_skipped * 100 /
                                    ((uint64_t)frames_done * MAC_CPU_CYCLES_PER_FRAME)),
                         (unsigned)ff_stat_count, (unsigned)ff_stat_abf7, (unsigned)ff_stat_tick);
                ff_stat_skipped = ff_stat_count = ff_stat_abf7 = ff_stat_tick = 0;
#endif
#if MAC_ROM_LC_II
                extern uint32_t v8_iwm_reads;
                ESP_LOGI(TAG, "IWMR: %lu reads", (unsigned long)v8_iwm_reads);
                v8_iwm_reads = 0;
#endif
                if (frame_count >= hist_report_at && pc_samples > 0) {
#if MAC_ROM_LC_II
                    uint32_t hotpc = 0;
                    for (int rank = 0; rank < 3; rank++) {
                        int best = -1;
                        uint32_t bestv = 0;
                        for (int i = 0; i < 16; i++)
                            if (v8_pv[i] > bestv) { bestv = v8_pv[i]; best = i; }
                        if (best < 0 || bestv == 0) break;
                        if (rank == 0) hotpc = (v8_pk[best] - 1) << 4;
                        ESP_LOGI(TAG, "PC-EXACT: pc=%06X share=%d%% count=%lu",
                                 ((v8_pk[best] - 1) << 4), (int)(bestv * 100 / pc_samples), (unsigned long)bestv);
                        v8_pv[best] = 0;
                    }
                    for (int i = 0; i < 16; i++) v8_pv[i] = 0;
                    {
                        uint8_t *rdh = mac_mem_get_ram();
                        bool allzero = false;
                        if (rdh && hotpc && hotpc < 0x800000) {
                            allzero = true;
                            char hexbuf[3 * 32 + 1];
                            int o = 0;
                            for (int bi = 0; bi < 32; bi++) {
                                uint8_t b = rdh[((hotpc + bi) & 0x1FFFFF)];
                                if (b) allzero = false;
                                hexbuf[o++] = "0123456789ABCDEF"[b >> 4];
                                hexbuf[o++] = "0123456789ABCDEF"[b & 15];
                                hexbuf[o++] = ' ';
                            }
                            hexbuf[o] = 0;
                            ESP_LOGI(TAG, "HOTBYTES @%06X: %s", hotpc, hexbuf);
                        }
                        if (allzero) {
                            char ringbuf[16 * 7 + 1];
                            int o = 0;
                            for (int k = 0; k < 16; k++) {
                                uint32_t p = v8_ring[(v8_ri + k) & 31];
                                for (int d = 5; d >= 0; d--)
                                    ringbuf[o++] = "0123456789ABCDEF"[(p >> (4 * d)) & 15];
                                ringbuf[o++] = ' ';
                            }
                            ringbuf[o] = 0;
                            ESP_LOGI(TAG, "RING: %s", ringbuf);
                            if (rdh) {
                                uint32_t prev = 0;
                                bool prev_zero = false;
                                for (int k = 0; k < 16; k++) {
                                    uint32_t p = v8_ring[(v8_ri + k) & 31];
                                    bool z = false;
                                    if (p < 0x800000) {
                                        z = true;
                                        for (int bi = 0; bi < 16; bi++)
                                            if (rdh[(p + bi) & 0x1FFFFF]) { z = false; break; }
                                    }
                                    if (z && !prev_zero && prev) {
                                        ESP_LOGI(TAG, "ZEROIN: prev=%06X pc=%06X", prev, p);
                                        break;
                                    }
                                    prev = p;
                                    prev_zero = z;
                                }
                            }
                        }
                    }
                    {
                        int seen[3];
                        for (int rank = 0; rank < 3; rank++) {
                            int best = -1;
                            uint32_t bestv = 0;
                            for (int i = 0; i < 512; i++) {
                                int dup = 0;
                                for (int s = 0; s < rank; s++)
                                    if (seen[s] == i) { dup = 1; break; }
                                if (!dup && v8_page[i] > bestv) { bestv = v8_page[i]; best = i; }
                            }
                            if (best < 0 || bestv == 0) break;
                            seen[rank] = best;
                            uint32_t base = (best < 500) ? ((uint32_t)best << 14)
                                                         : (0xA00000u + ((uint32_t)(best - 500) << 14));
                            ESP_LOGI(TAG, "PAGE: base=%06X count=%lu share=%d%%",
                                     base, (unsigned long)bestv,
                                     (int)(v8_ps_cum ? bestv * 100 / v8_ps_cum : 0));
                        }
                    }
#else
                    int seen_idx[10];
                    for (int rank = 0; rank < 10; rank++) {
                        int best = -1;
                        uint32_t bestv = 0;
                        for (int i = 0; i < 640; i++) {
                            int dup = 0;
                            for (int s = 0; s < rank; s++)
                                if (seen_idx[s] == i) { dup = 1; break; }
                            if (!dup && pc_hist[i] > bestv) { bestv = pc_hist[i]; best = i; }
                        }
                        if (best < 0 || bestv == 0) break;
                        seen_idx[rank] = best;
                        uint32_t page = (best < 512) ? ((uint32_t)best << 12)
#if MAC_ROM_LC_II
                            : (0xA00000u + ((uint32_t)(best - 512) << 12));
#else
                            : (0x400000u + ((uint32_t)(best - 512) << 12));
#endif
                        ESP_LOGI(TAG, "PC-HIST: page=%06X share=%d%% count=%lu",
                                 page, (int)(bestv * 100 / pc_samples), (unsigned long)bestv);
                    }
#endif
                    ESP_LOGI(TAG, "BADPC: %lu bad of %d samples",
                             (unsigned long)pc_bad_count, pc_samples);
                    hist_report_at = frame_count + (mac_post_boot ? 3000 : 600);
                }
                /* Profiling breakdown: exec vs overhead */
                if (prof_frame_count > 0 && prof_total_sum > 0) {
                    uint64_t overhead = (prof_total_sum > prof_exec_sum)
                                        ? prof_total_sum - prof_exec_sum : 0;
                    int exec_pct = (int)(prof_exec_sum * 100 / prof_total_sum);
                    ESP_LOGI(TAG, "PROFILE: %d frames, exec=%d%% overhead=%d%% (exec=%lluM oh=%lluM cycles)",
                             prof_frame_count, exec_pct, 100 - exec_pct,
                             (unsigned long long)(prof_exec_sum / 1000000),
                             (unsigned long long)(overhead / 1000000));
                    prof_exec_sum = 0;
                    prof_total_sum = 0;
                    prof_frame_count = 0;
                }
                speed_base_us = esp_timer_get_time();
                speed_base_frame = frame_count;
            }
        }

        /* Touch input: trackpad-style gestures (mac_touch.c).
         * - Swipe/drag: moves cursor, NO button press
         * - Quick tap (< 300ms, < 8px movement): generates click
         * - Tap-then-drag: tap once, then touch again within 400ms
         *   to click-and-drag (for moving windows/icons)
         * The panel is sampled by its own task on the ESP32
         * (TOUCH_SAMPLER_TASK) or inline here every 16 ms (host); the loop
         * consumes the accumulated movement/button once per frame. */
        {
            touch_poll_inline(esp_timer_get_time());
            touch_report_t tr;
            touch_consume(&tr);
#if MAC_KEYBOARD
            kbd_frame();
#endif
#if TOUCH_GAME_MODE
            game_mode_track(mac_mem_get_ram());
#endif
#if GAME_FULL_VIEW && TOUCH_GAME_MODE
            full_view_track(mac_mem_get_ram(), frame_count);
            full_view_scale(&tr.dx, &tr.dy);
#endif
#if V8GFV
            full_view_scale(&tr.dx, &tr.dy);
#endif
#if MAC_ROM_LC_II
            v8_apply_touch(&tr, frame_count);
#else
            if (tr.dx || tr.dy || tr.button != mouse_button)
                mac_emu_set_mouse(mouse_x + tr.dx, mouse_y + tr.dy, tr.button);
#endif
            {
                static bool prev_touch, prev_btn;
                static int tlog;
                if ((tr.touching != prev_touch || tr.button != prev_btn) && tlog < 60) {
                    tlog++;
                    ESP_LOGI(TAG, "TOUCH F%d %s btn=%d drag=%d tapdrag=%d",
                             frame_count, tr.touching ? "down" : "up", tr.button,
                             tr.is_drag, tr.tap_drag);
                }
                prev_touch = tr.touching;
                prev_btn = tr.button;
            }
        }

#if MAC_IDLE_FF
        /* Frame-end updates (touch -> Mouse globals/events, V8 disk) can
         * change what the idle loop sees: count them as an event. */
        mac_idle_epoch++;
#endif

#if MAC_FRAME_THROTTLE
        /* Real-time cap: at most one emulated frame per MAC_FRAME_US of wall
         * clock. Only matters when frames are cheap (idle fast-forward); a
         * frame that ran late is not caught up by more than ~100 ms. */
        {
            static int64_t thr_base;
            static int64_t thr_frames;
#if MAC_BOOT_TURBO
            /* Uncapped until the Finder has been in front for 2 s (or 30000
             * frames at most): the boot runs as fast as the host allows. */
            static int turbo_left = 30000, finder_frames;
            bool turbo = false;
            if (turbo_left > 0) {
                uint8_t *r = mac_mem_get_ram();
                uint32_t wmgr = r ? rd32(r, 0x09DE) : 0;
                int al = r ? r[0x0910] : 0, fl = r ? r[0x02E0] : 0;
                bool finder = r && mac_post_boot && wmgr != 0 && wmgr != 0xFFFFFFFF &&
                              al == fl && al > 0 && al < 32 &&
                              memcmp(r + 0x0911, r + 0x02E1, al) == 0;
                finder_frames = finder ? finder_frames + 1 : 0;
                if (--turbo_left == 0 || finder_frames >= 120) {
                    turbo_left = 0;
                    ESP_LOGI(TAG, "BOOT-TURBO: off at frame %d (%lld ms)", frame_count,
                             (long long)(esp_timer_get_time() / 1000));
                }
                /* Queued audio (the start-up chime) must play at its own
                 * speed: stay capped while the consumer has a backlog. */
                turbo = !asc_backlog();
                if (turbo) {
                    thr_base = 0;               /* restart the cap afterwards */
                    thr_frames = 0;
                }
            }
            if (!turbo)
#endif
            {
            int64_t now = esp_timer_get_time();
            if (thr_base == 0) thr_base = now;
            thr_frames++;
            int64_t target = thr_base + thr_frames * MAC_FRAME_US;
            if (now < target) {
                int64_t ms = (target - now) / 1000;
                if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms));
            } else if (now - target > 100000) {
                thr_base = now;
                thr_frames = 0;
            }
            }
        }
#endif

        /* Yield periodically to prevent watchdog timeout. */
        taskYIELD();
    }
}

const uint8_t *mac_emu_get_screen(void)
{
    uint8_t *ram = mac_mem_get_ram();
    if (!ram) return NULL;
    return ram + (mac_mem_get_ram_size() - 0x5900);
}

void mac_emu_set_mouse(int x, int y, bool button)
{
    if (x < 0) x = 0;
    if (x >= mac_vis_w) x = mac_vis_w - 1;      /* 480x320, or 512x342 in */
    if (y < 0) y = 0;                           /* the game full view     */
    if (y >= mac_vis_h) y = mac_vis_h - 1;

    mouse_x = x;
    mouse_y = y;
    mouse_button = button;
}
