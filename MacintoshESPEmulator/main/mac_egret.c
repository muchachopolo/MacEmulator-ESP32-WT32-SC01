/*
 * Egret / Cuda microcontroller HLE for V8-family Macs.
 *
 * The Color Classic has a Cuda (its ProductInfo $A03858 selects Cuda
 * primitives and the OS installs the Cuda manager $ACFD40 as _EgretDispatch);
 * LC II-class V8 boxes use Egret ($AB382E PRAM primitive, $A14800 manager).
 * Both use VIA1 PB3 = TREQ/XcvrSes (to CPU, active low) and move bytes
 * through the VIA shift register (ACR bit 4: 1 = CPU shifts out); each byte
 * completed sets IFR.SR. They differ on the CPU-driven lines:
 *   Egret: PB5 SysSes active high, PB4 ViaFull pulsed high per byte
 *          (rise = command byte ready; fall = CPU wants the next reply byte).
 *   Cuda:  PB5 TIP active low, PB4 BYTEACK toggled once per byte.
 * Session: CPU asserts TIP with byte 0 in SR (shift-out), sends the rest one
 * handshake each, then shift-in + negate TIP. MCU asserts TREQ and shifts an
 * attention byte (discarded). CPU asserts TIP -> reply byte 0; each further
 * handshake -> next byte; TREQ is negated with the last byte. CPU negates
 * TIP -> idle. Reply packet: [type][flag][cmd][data...], type 1 = pseudo.
 *
 * One-second tick: the MCU, not the VIA, keeps the clock. Each second it
 * sends an unsolicited tick packet [03][00][00] (type 3 = TIMER_PACKET in
 * Linux adb.h). The ROM's unsolicited-packet path (LC II $A14A88: type 3 ->
 * Egret globals $28 = $A14BE2; Color Classic Cuda $AD026A) jumps through
 * Lvl1DT[0], the one-second handler $A0B140 (ADDQ.L #1,Time). The VIA1
 * CA2 flag that via_one_second() sets is never taken on V8 (that handler
 * is not reached), so without these packets Time ($20C) never advances. Started once the ROM has sent SET_ONE_SECOND_MODE ($1B):
 * Cuda (Color Classic sends mode 3) when the mode is != 0; Egret (LC II
 * sends mode 0 early in boot) while autopoll is on, i.e. once the
 * interrupt-driven manager runs (Linux sees Egret timer packets after
 * enabling autopoll). Earlier, the ROM's polled PRAM code ($A15392) raises
 * SysSes without looking at TREQ and would read a tick as its reply.
 */
#include "mac_egret.h"
#include "mac_kbd.h"
#include "mac_via.h"
#include <stdio.h>
#include <string.h>
#include <esp_log.h>

#if MAC_ROM_LC_II

static const char *TAG = "egret";

#define EG_TIP      0x20
#define EG_BYTEACK  0x10
#define EG_DELAY    64      /* CPU cycles before the MCU answers an edge */
#define EG_CUDA     MAC_V8_CUDA     /* Cuda line polarities (Color Classic) */
#ifndef EGRET_ONE_SEC_TICKS             /* 1 = send the 1 Hz tick packets */
#define EGRET_ONE_SEC_TICKS 1
#endif

enum { EG_IDLE, EG_CMD, EG_ATTN, EG_REPLY, EG_SYNC };
enum { ACT_NONE, ACT_OUT_DONE, ACT_ATTN, ACT_BYTE, ACT_SYNC_ON, ACT_SYNC_OFF, ACT_SR_IRQ };

static struct {
    int state;
    bool tip;                   /* session line asserted (polarity-free) */
    uint8_t ack;                /* last seen PB4 level */
    uint8_t cmd[32];
    int cmd_len;
    uint8_t reply[40];
    int reply_len, reply_idx;
    int action, delay;          /* one pending Egret action */
    uint8_t pram[256];
    uint32_t rtc;               /* Mac time, seconds since 1904 */
    bool autopoll;
    bool stream;                /* Egret READ_PRAM: keep returning bytes */
    uint8_t stream_addr;
    int log_cnt;
    bool unsolicited;           /* reply[] holds an unsolicited packet */
    bool unsol_tick;            /* ... and it is a tick packet, not ADB */
    bool unsol_kbd;             /* ... an ADB keyboard packet (MAC_KEYBOARD) */
    bool sec_mode_set;          /* SET_ONE_SECOND_MODE seen */
    uint8_t sec_mode;           /* its argument */
    int ticks_pending;          /* one-second ticks not yet delivered */
    bool list_set;              /* SET_DEVICE_LIST seen */
    uint16_t dev_mask;          /* autopoll device list */
    int poll_wait;              /* cycles until the next autopoll packet */
} eg;

/* ADB mouse (relative, handler 1 = 100 cpi). Deltas accumulate between
 * packets; button transitions are queued so a quick tap still produces a
 * down packet followed by an up packet. */
#define EG_POLL_CYCLES 65000    /* min gap between autopoll packets (~1/2 frame) */
static struct {
    uint8_t addr, handler;
    int dx, dy;                 /* accumulated, not yet reported */
    bool btn;                   /* last state reported to the Mac */
    bool btn_req;               /* last state requested by the host */
    uint8_t q[8];               /* pending button states */
    int qh, qn;
    int in_dx, in_dy;           /* deltas in the packet being sent */
    bool in_btn_pop;            /* packet consumed a queued button state */
    int log_cnt;
} ms;

static void mouse_reset(void)
{
    ms.addr = 3;
    ms.handler = 1;
    ms.dx = ms.dy = 0;
    ms.qn = 0;
}

static bool mouse_pending(void)
{
    return ms.dx || ms.dy || ms.qn;
}

static int clamp63(int v) { return v > 63 ? 63 : v < -63 ? -63 : v; }

/* Build Talk R0 data (2 bytes) from the accumulated state. */
static void mouse_take_r0(uint8_t out[2])
{
    int dx = clamp63(ms.dx), dy = clamp63(ms.dy);
    ms.dx -= dx;
    ms.dy -= dy;
    ms.in_dx = dx;
    ms.in_dy = dy;
    ms.in_btn_pop = false;
    if (ms.qn) {
        ms.btn = ms.q[ms.qh];
        ms.qh = (ms.qh + 1) & 7;
        ms.qn--;
        ms.in_btn_pop = true;
    }
    out[0] = (ms.btn ? 0x00 : 0x80) | (dy & 0x7F);
    out[1] = 0x80 | (dx & 0x7F);
}

/* An autopoll packet was abandoned (command collision): give it back. */
static void mouse_untake(void)
{
    ms.dx += ms.in_dx;
    ms.dy += ms.in_dy;
    if (ms.in_btn_pop && ms.qn < 8) {
        ms.qh = (ms.qh + 7) & 7;
        ms.q[ms.qh] = ms.btn;
        ms.qn++;
        ms.btn = !ms.btn;
    }
    ms.in_dx = ms.in_dy = 0;
    ms.in_btn_pop = false;
}

void egret_adb_mouse(int dx, int dy, bool button)
{
    ms.dx += dx;
    ms.dy += dy;
    /* keep the backlog bounded (finger flicks faster than the OS polls) */
    if (ms.dx > 512) ms.dx = 512; else if (ms.dx < -512) ms.dx = -512;
    if (ms.dy > 512) ms.dy = 512; else if (ms.dy < -512) ms.dy = -512;
    if (button != ms.btn_req) {
        ms.btn_req = button;
        if (ms.qn < 8) {
            ms.q[(ms.qh + ms.qn) & 7] = button;
            ms.qn++;
        }
    }
}

bool egret_adb_mouse_btn_pending(void)
{
    return ms.qn > 0;
}

void egret_init(void)
{
    static bool powered;
    if (!powered) {             /* power-on: PRAM + clock */
        powered = true;
        memset(&eg, 0, sizeof(eg));
        eg.rtc = 0xE6FC5F80;    /* 2026-10-20 00:00:00 */
        /* XPRAM $01 low 5 bits = SCSI startup-disk wait in seconds; 0 means
         * the ROM's 20 s default before it falls back to the floppy */
        eg.pram[0x01] = 1;
        mouse_reset();
        ESP_LOGI(TAG, "%s HLE initialized", EG_CUDA ? "Cuda" : "Egret");
    }
    /* VIA reset (68K RESET): drop any session, keep PRAM/RTC */
    eg.state = EG_IDLE;
    eg.tip = false;
    eg.ack = EG_CUDA ? EG_BYTEACK : 0;
    eg.action = ACT_NONE;
    eg.unsolicited = false;
    eg.unsol_tick = false;
    eg.ticks_pending = 0;
    eg.sec_mode_set = false;
    via_egret_set_treq(false);
}

/* An unsolicited packet was abandoned (command collision / TIP dropped
 * before the last byte): give its content back for a retry. */
static void unsolicited_untake(void)
{
    if (eg.unsol_tick) {
        if (eg.ticks_pending < 4) eg.ticks_pending++;
    } else if (eg.unsol_kbd) {
        kbd_adb_untake();
    } else {
        mouse_untake();
    }
    eg.unsol_tick = false;
    eg.unsol_kbd = false;
}

static void schedule(int action)
{
    eg.action = action;
    eg.delay = EG_DELAY;
}

static void reply_hdr(uint8_t type, uint8_t flag, uint8_t cmd)
{
    eg.reply[0] = type;
    eg.reply[1] = flag;
    eg.reply[2] = cmd;
    eg.reply_len = 3;
}

static void put_byte(uint8_t b)
{
    if (eg.reply_len < (int)sizeof(eg.reply)) eg.reply[eg.reply_len++] = b;
}

/* ADB command byte: aaaa kk rr. kk=11 Talk, 10 Listen; 0000 SendReset,
 * xxxx0001 Flush. Reply [00][flag][cmd][data]; flag $02 = no response. */
static void adb_command(uint8_t cmd, const uint8_t *a, int na)
{
    uint8_t addr = cmd >> 4, reg = cmd & 3, kind = (cmd >> 2) & 3;
    uint8_t flag = 0x02;
    uint8_t data[2];
    int nd = 0;

    if ((cmd & 0x0F) == 0x00) {                 /* SendReset */
        mouse_reset();
#if MAC_KEYBOARD
        kbd_adb_reset();
#endif
        flag = 0x00;
#if MAC_KEYBOARD
    } else if (addr == kbd_adb_addr() && addr != ms.addr) {
        /* ADB keyboard: Talk R0 = up to two key codes (bit 7 = up),
         * Talk R2 = modifier/LED state (all released), Talk R3 = id. */
        if ((cmd & 0x0F) == 0x01) {             /* Flush */
            flag = 0x00;
        } else if (kind == 3 && reg == 3) {
            data[0] = 0x60 | addr;
            data[1] = kbd_adb_handler();
            nd = 2;
            flag = 0x00;
        } else if (kind == 3 && reg == 2) {
            data[0] = 0xFF;
            data[1] = 0xFF;
            nd = 2;
            flag = 0x00;
        } else if (kind == 3 && reg == 0) {
            if (kbd_adb_pending()) {
                kbd_adb_take_r0(data);
                nd = 2;
                flag = 0x00;
            }
        } else if (kind == 2 && reg == 3 && na >= 2) {
            kbd_adb_listen_r3(a[0], a[1]);
            flag = 0x00;
        } else if (kind == 2) {
            flag = 0x00;
        }
#endif
    } else if (addr != ms.addr) {
        flag = (kind == 2) ? 0x00 : 0x02;       /* Listen gets no answer anyway */
    } else if ((cmd & 0x0F) == 0x01) {          /* Flush */
        ms.dx = ms.dy = 0;
        flag = 0x00;
    } else if (kind == 3 && reg == 3) {         /* Talk R3: addr + handler */
        data[0] = 0x60 | ms.addr;
        data[1] = ms.handler;
        nd = 2;
        flag = 0x00;
    } else if (kind == 3 && reg == 0) {         /* Talk R0: movement */
        if (mouse_pending()) {
            mouse_take_r0(data);
            nd = 2;
            flag = 0x00;
        }
    } else if (kind == 2 && reg == 3 && na >= 2) {  /* Listen R3 */
        uint8_t h = a[1];
        if (h == 0x00 || h == 0xFD || h == 0xFE)
            ms.addr = a[0] & 0x0F;
        else if (h == 0x01 || h == 0x02)
            ms.handler = h;
        flag = 0x00;
    } else if (kind == 2) {
        flag = 0x00;
    }
    reply_hdr(0x00, flag, cmd);
    for (int i = 0; i < nd; i++) put_byte(data[i]);
    if (ms.log_cnt < 80 && (cmd & 0x0F) != 0x0C) {
        ms.log_cnt++;
        ESP_LOGI(TAG, "ADB %02X [%02X %02X] -> flag %02X [%02X %02X] (mouse addr %d handler %d)",
                 cmd, na > 0 ? a[0] : 0, na > 1 ? a[1] : 0, flag,
                 nd ? data[0] : 0, nd ? data[1] : 0, ms.addr, ms.handler);
    }
}

static void process_command(void)
{
    uint8_t type = eg.cmd[0], cmd = eg.cmd_len > 1 ? eg.cmd[1] : 0;
    const uint8_t *a = &eg.cmd[2];
    int na = eg.cmd_len - 2;
    bool known = true;

    eg.stream = false;
    if (type == 0x00) {
        adb_command(cmd, a, na);
    } else if (type == 0x01) {
        reply_hdr(0x01, 0x00, cmd);
        switch (cmd) {
        case 0x01:                          /* START/STOP AUTOPOLL */
            if (eg.autopoll != (na > 0 && a[0]))
                ESP_LOGI(TAG, "ADB autopoll %s", (na > 0 && a[0]) ? "on" : "off");
            eg.autopoll = na > 0 && a[0];
            break;
        case 0x02: {                        /* READ 6805 MEM addr.w */
            /* XPRAM lives at 6805 $0100-$01FF (ROM checks $0110=$A8,
             * $010C="NuMc" this way); other RAM reads as 0. */
            uint16_t ad = na >= 2 ? (a[0] << 8) | a[1] : 0;
            for (int i = 0; i < 4; i++, ad++)
                put_byte((ad & 0xFF00) == 0x0100 ? eg.pram[ad & 0xFF] : 0);
            break;
        }
        case 0x08: {                        /* WRITE 6805 MEM addr.w data... */
            uint16_t ad = na >= 2 ? (a[0] << 8) | a[1] : 0;
            for (int i = 2; i < na; i++, ad++)
                if ((ad & 0xFF00) == 0x0100) eg.pram[ad & 0xFF] = a[i];
            break;
        }
        case 0x22:                          /* GET/SET IIC: no devices */
            break;
        case 0x1B:                          /* SET_ONE_SECOND_MODE mode */
            eg.sec_mode_set = true;
            eg.sec_mode = na > 0 ? a[0] : 0;
            eg.ticks_pending = 0;
            break;
        case 0x19:                          /* SET_DEVICE_LIST mask.w */
            eg.list_set = true;
            eg.dev_mask = na >= 2 ? (uint16_t)((a[0] << 8) | a[1]) : 0;
            ESP_LOGI(TAG, "ADB autopoll device list = %04X", eg.dev_mask);
            break;
        case 0x03:                          /* GET_TIME */
            put_byte(eg.rtc >> 24); put_byte(eg.rtc >> 16);
            put_byte(eg.rtc >> 8);  put_byte(eg.rtc);
            break;
        case 0x07: {                        /* READ_PRAM addr.w */
            uint8_t ad = na >= 2 ? a[1] : 0;
            put_byte(eg.pram[ad]);
            /* Egret streams consecutive PRAM bytes for as long as the host
             * keeps reading (LC II $A15346 reads 32 bytes from $00 in one
             * session).  Cuda (Color Classic) path keeps the 1-byte reply. */
            if (!EG_CUDA) {
                eg.stream = true;
                eg.stream_addr = (uint8_t)(ad + 1);
            }
            break;
        }
        case 0x09:                          /* SET_TIME */
            if (na >= 4)
                eg.rtc = ((uint32_t)a[0] << 24) | (a[1] << 16) | (a[2] << 8) | a[3];
            break;
        case 0x0C: {                        /* WRITE_PRAM addr.w data... */
            uint8_t ad = na >= 2 ? a[1] : 0;
            for (int i = 2; i < na; i++) eg.pram[(uint8_t)(ad + i - 2)] = a[i];
            break;
        }
        default:
            known = false;
            break;
        }
    } else {
        reply_hdr(type, 0x00, cmd);
        known = false;
    }

    /* Log the first 48 commands plus the first use of every command */
    static uint32_t seen[2][8];
    uint32_t *sw = &seen[type & 1][cmd >> 5];
    bool first = !(*sw & (1u << (cmd & 31)));
    *sw |= 1u << (cmd & 31);
    if (first || eg.log_cnt++ < 48) {
        char hex[3 * 12 + 1] = {0};
        for (int i = 0; i < eg.cmd_len && i < 12; i++)
            snprintf(hex + 3 * i, 4, "%02X ", eg.cmd[i]);
        char rhex[3 * 12 + 1] = {0};
        for (int i = 0; i < eg.reply_len && i < 12; i++)
            snprintf(rhex + 3 * i, 4, "%02X ", eg.reply[i]);
        if (known) ESP_LOGI(TAG, "CMD %s-> %s", hex, rhex);
        else       ESP_LOGW(TAG, "CMD %s(unhandled) -> %s", hex, rhex);
    }
}

void egret_portb_changed(uint8_t orb, uint8_t ddrb)
{
    /* Undriven (input) pins float to the MCU's idle level */
    uint8_t idle = EG_CUDA ? (EG_TIP | EG_BYTEACK) : 0;
    uint8_t pb = (orb & ddrb) | (idle & ~ddrb);
    bool tip = EG_CUDA ? !(pb & EG_TIP) : (pb & EG_TIP) != 0;
    uint8_t ack = pb & EG_BYTEACK;
    bool tip_on = !eg.tip && tip, tip_off = eg.tip && !tip;
    bool ack_edge = ack != eg.ack;
    /* Byte handshake: Cuda = any BYTEACK toggle; Egret = ViaFull rise for
     * command bytes, fall for reply bytes. */
    bool hs_out = EG_CUDA ? ack_edge : (ack_edge && ack);
    bool hs_in  = EG_CUDA ? ack_edge : (ack_edge && !ack);
    eg.tip = tip;
    eg.ack = ack;
    bool shift_out = (via_get_acr() & 0x10) != 0;

    if (tip_on) {
        if (eg.state == EG_ATTN && eg.unsolicited && shift_out) {
            /* Collision: the CPU starts a command while our autopoll
             * packet waits. Drop the packet (data goes back) and take
             * the command. */
            unsolicited_untake();
            eg.unsolicited = false;
            eg.action = ACT_NONE;
            eg.state = EG_IDLE;
        }
        if (eg.state == EG_ATTN) {
            eg.state = EG_REPLY;            /* CPU accepts the reply */
            eg.reply_idx = 0;
            schedule(ACT_BYTE);
        } else if (eg.state == EG_IDLE || eg.state == EG_SYNC) {
            via_egret_set_treq(false);
            eg.state = EG_CMD;
            eg.cmd_len = 0;
            if (EG_CUDA && shift_out) {     /* Cuda: byte 0 rides on TIP */
                eg.cmd[eg.cmd_len++] = via_get_sr();
                schedule(ACT_OUT_DONE);
            }
        }
        return;
    }
    if (tip_off) {
        if (eg.state == EG_CMD) {
            if (eg.cmd_len >= 2) {
                eg.unsolicited = false;
                eg.unsol_tick = false;
                process_command();
                eg.state = EG_ATTN;
                schedule(ACT_ATTN);
            } else {
                eg.state = EG_IDLE;
            }
        } else if (eg.state == EG_REPLY || eg.state == EG_ATTN) {
            if (eg.unsolicited && eg.reply_idx < eg.reply_len)
                unsolicited_untake();       /* packet not fully read */
            eg.unsolicited = false;
            eg.state = EG_IDLE;             /* session over (or aborted) */
            eg.action = ACT_NONE;
            via_egret_set_treq(false);
            /* Cuda raises one more SR irq when TIP drops ($AB3BA6) */
            if (EG_CUDA) schedule(ACT_SR_IRQ);
        }
        return;
    }
    if (!tip) {
        /* Cuda sync ($AD0288, as Linux cuda_init_via): BYTEACK asserted
         * without TIP -> TREQ + SR irq; BYTEACK negated -> TREQ off + SR irq */
        if (EG_CUDA && ack_edge) {
            if (!ack && eg.state == EG_IDLE) {
                eg.state = EG_SYNC;
                schedule(ACT_SYNC_ON);
            } else if (ack && eg.state == EG_SYNC) {
                eg.state = EG_IDLE;
                schedule(ACT_SYNC_OFF);
            }
        }
        return;
    }
    if (hs_out && eg.state == EG_CMD && shift_out) {
        if (eg.cmd_len < (int)sizeof(eg.cmd))
            eg.cmd[eg.cmd_len++] = via_get_sr();
        schedule(ACT_OUT_DONE);
    } else if (hs_in && eg.state == EG_REPLY && eg.reply_idx < eg.reply_len) {
        schedule(ACT_BYTE);
    }
}

/* Unsolicited packets, through the attention handshake:
 *  - one-second tick [03][00][00] (see the header comment);
 *  - autopoll: with the mouse holding data, [00][40][addr<<4|$C][b0][b1]. */
static void autopoll(int cycles)
{
    if (eg.poll_wait > 0) eg.poll_wait -= cycles;
    if (eg.state != EG_IDLE || eg.tip)
        return;
    if (eg.ticks_pending > 0) {
        eg.ticks_pending--;
        eg.stream = false;
        reply_hdr(0x03, 0x00, 0x00);
        eg.unsolicited = true;
        eg.unsol_tick = true;
        eg.unsol_kbd = false;
        eg.state = EG_ATTN;
        schedule(ACT_ATTN);
        return;
    }
    if (eg.poll_wait > 0 || !eg.autopoll)
        return;
#if MAC_KEYBOARD
    /* Keyboard first: keys are rare, and a click must not wait behind a
     * stream of mouse packets either way (each packet takes one poll). */
    if (kbd_adb_pending() &&
        !(eg.list_set && !(eg.dev_mask & (1u << kbd_adb_addr())))) {
        uint8_t k[2];
        eg.stream = false;
        reply_hdr(0x00, 0x40, (uint8_t)((kbd_adb_addr() << 4) | 0x0C));
        kbd_adb_take_r0(k);
        put_byte(k[0]);
        put_byte(k[1]);
        eg.unsolicited = true;
        eg.unsol_tick = false;
        eg.unsol_kbd = true;
        eg.state = EG_ATTN;
        eg.poll_wait = EG_POLL_CYCLES;
        schedule(ACT_ATTN);
        return;
    }
#endif
    if (!mouse_pending())
        return;
    if (eg.list_set && !(eg.dev_mask & (1u << ms.addr)))
        return;
    uint8_t d[2];
    eg.stream = false;
    reply_hdr(0x00, 0x40, (uint8_t)((ms.addr << 4) | 0x0C));
    mouse_take_r0(d);
    put_byte(d[0]);
    put_byte(d[1]);
    static int pkt_log;
    if (pkt_log < 100 || (ms.in_btn_pop && pkt_log < 400)) {
        pkt_log++;
        ESP_LOGI(TAG, "ADB autopoll pkt %02X %02X (left dx=%d dy=%d q=%d)",
                 d[0], d[1], ms.dx, ms.dy, ms.qn);
    }
    eg.unsolicited = true;
    eg.unsol_tick = false;
    eg.unsol_kbd = false;
    eg.state = EG_ATTN;
    eg.poll_wait = EG_POLL_CYCLES;
    schedule(ACT_ATTN);
}

void egret_tick(int cycles)
{
    if (eg.action == ACT_NONE) {
        autopoll(cycles);
        return;
    }
    eg.delay -= cycles;
    if (eg.delay > 0) return;
    int act = eg.action;
    eg.action = ACT_NONE;
    switch (act) {
    case ACT_OUT_DONE:
        via_egret_shift_done(false, 0);
        break;
    case ACT_ATTN:
        via_egret_set_treq(true);
        via_egret_shift_done(true, 0x00);
        break;
    case ACT_SYNC_ON:
        via_egret_set_treq(true);
        via_egret_shift_done(true, 0x00);
        break;
    case ACT_SYNC_OFF:                      /* TREQ drops first ($AD02EE), */
        via_egret_set_treq(false);
        schedule(ACT_SR_IRQ);               /* then the SR irq ($AD0308) */
        break;
    case ACT_SR_IRQ:
        via_egret_shift_done(true, 0x00);
        break;
    case ACT_BYTE: {
        uint8_t b = eg.reply[eg.reply_idx++];
        if (eg.stream && eg.reply_idx >= eg.reply_len) {
            eg.reply_idx = eg.reply_len = 0;    /* refill with next PRAM byte */
            put_byte(eg.pram[eg.stream_addr++]);
        } else if (eg.reply_idx >= eg.reply_len)
            via_egret_set_treq(false);      /* last byte */
        via_egret_shift_done(true, b);
        break;
    }
    }
}

void egret_one_second(void)
{
    eg.rtc++;
    /* Queue a tick packet (a few at most: a session may hold off delivery) */
    if (EGRET_ONE_SEC_TICKS && eg.sec_mode_set &&
        (EG_CUDA ? eg.sec_mode != 0 : eg.autopoll) && eg.ticks_pending < 4)
        eg.ticks_pending++;
}

#endif /* MAC_ROM_LC_II */
