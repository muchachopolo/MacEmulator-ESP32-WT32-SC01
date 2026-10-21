/*
 * Classic Mac RTC / PRAM chip emulation (Mac Plus/SE/Classic, 343-0042-B).
 *
 * Serial protocol on VIA port B (Guide to the Macintosh Family Hardware,
 * "Real-Time Clock"):
 *   PB2 /rtcEnb  low = transaction in progress; high aborts / ends it.
 *   PB1 rtcClk   the CPU clocks every bit; the chip samples rtcData on the
 *                rising edge and, when sending, puts the next bit on rtcData
 *                after the falling edge (the ROM reads PB0 after raising
 *                the clock again).
 *   PB0 rtcData  MSB first.  CPU-driven (DDRB bit 0 = 1) for command and
 *                write bytes, chip-driven (DDRB bit 0 = 0) for read bytes.
 *
 * Command byte  z a4 a3 a2 a1 a0 x x   (z = 1 read, 0 write), address
 * a = bits 6..2:
 *   0-3   seconds bytes 0..3 (LSB first); 4-7 mirror them
 *   8-11  PRAM $10-$13 (z010aa01)      -> XPRAM $08-$0B
 *   12    test register (write only, 00110001)
 *   13    write-protect register (write only, 00110101; bit 7 = protect)
 *   14-15 extended command z0111aaa: a second byte 0bbbbb00 follows,
 *         XPRAM address = aaa:bbbbb (256 bytes)
 *   16-31 PRAM $00-$0F (z1aaaa01)      -> XPRAM $10-$1F
 * so the 20 classic PRAM bytes sit at XPRAM $08-$0B and $10-$1F.  The
 * Classic ROM keeps SysParam (low memory $1F8-$20B) bytes 0-15 (SPValid ..
 * SPPrint) in the "$00-$0F" group = XPRAM $10-$1F and bytes 16-19
 * (SPVolCtl .. SPMisc2) in the "$10-$13" group = XPRAM $08-$0B.
 * The seconds counter advances on the one-second tick (via_one_second()).
 * Classic build only (the V8 build keeps PRAM/clock in the Egret HLE).
 */
#include "mac_rtc.h"

#if CLASSIC_RTC && !MAC_ROM_LC_II
#include "m68k.h"
#include <string.h>
#include <esp_log.h>

static const char *TAG = "rtc";

/* 2026-10-21 00:00:00, seconds since 1904-01-01 (the V8 Egret HLE starts
 * at 2026-10-20). */
#define RTC_START_SECONDS 0xE6FDB100u

/* CLASSIC_RTC_PRAM_DEFAULTS (default 1): start with a valid PRAM image
 * (PRAM-reset defaults, AppleTalk off).  0 = all-zero image; the ROM sees
 * the bad signatures and runs its own PRAM initialisation. */
#ifndef CLASSIC_RTC_PRAM_DEFAULTS
#define CLASSIC_RTC_PRAM_DEFAULTS 1
#endif

/* SPConfig: high nibble port A use, low nibble port B use
 * (0 useFree, 1 useATalk, 2 useAsync). */
#ifndef CLASSIC_RTC_SPCONFIG
#define CLASSIC_RTC_SPCONFIG 0x22
#endif

/* Number of RTC transactions to log (0 = quiet) */
#ifndef RTC_LOG_N
#define RTC_LOG_N 8
#endif

enum { ST_CMD, ST_XADDR, ST_WDATA, ST_SEND, ST_DONE };

static struct {
    uint32_t seconds;
    uint8_t  pram[256];
    bool     wp;            /* write-protect register bit 7 */

    bool     en;            /* /rtcEnb low */
    int      clk;           /* last rtcClk level */
    int      state;
    int      bits;          /* bits shifted in / out of the current byte */
    uint8_t  shift;         /* incoming byte */
    uint8_t  out;           /* outgoing byte */
    int      dout;          /* level driven on rtcData */
    uint8_t  cmd;
    uint8_t  addr;          /* XPRAM address or register number */
    int      log_n;
} rtc;

static void macrtc_log(const char *what, int addr, uint8_t val)
{
    if (rtc.log_n >= RTC_LOG_N) return;
    rtc.log_n++;
    ESP_LOGI(TAG, "%s cmd=%02X addr=%02X val=%02X PC=%06X", what, rtc.cmd,
             addr, val, (unsigned)(m68k_get_reg(NULL, M68K_REG_PPC) & 0xFFFFFF));
}

void macrtc_init(void)
{
    memset(&rtc, 0, sizeof(rtc));
    rtc.seconds = RTC_START_SECONDS;
    rtc.clk = 1;
    rtc.dout = 1;
#if CLASSIC_RTC_PRAM_DEFAULTS
    /* The SysParam values the Classic ROM writes when it finds PRAM invalid
     * (logged on the host), except SPConfig: the ROM default $00 (both
     * ports useFree) lets System 7.5.3 open .MPP on the printer port and
     * set SPConfig to $01 (AppleTalk active); $22 (both ports useAsync)
     * keeps AppleTalk inactive (PortAUse/PortBUse $290/$291 stay $FF). */
    static const uint8_t sysparam[20] = {
        0xA8,       /* SPValid: validity signature                     */
        0x00,       /* SPATalkA: AppleTalk node hint, port A           */
        0x00,       /* SPATalkB                                        */
        CLASSIC_RTC_SPCONFIG, /* SPConfig (see above)                 */
        0xCC, 0x0A, /* SPPortA: 9600 baud, 8 data, 2 stop, no parity   */
        0xCC, 0x0A, /* SPPortB                                         */
        0x00, 0x00, 0x00, 0x00, /* SPAlarm                             */
        0x00, 0x02, /* SPFont: application font - 1 (Geneva)           */
        0x63,       /* SPKbd: repeat threshold 6, rate 3               */
        0x00,       /* SPPrint: printer port A                         */
        0x03,       /* SPVolCtl: speaker volume 3                      */
        0x88,       /* SPClikCaret: double-click 8, caret blink 8      */
        0x00,       /* SPMisc1                                         */
        0x4C,       /* SPMisc2: mouse scaling, menu blink count      */
    };
    /* The ROM keeps SysParam bytes 0-15 in the "$00-$0F" group (XPRAM
     * $10-$1F) and bytes 16-19 in the "$10-$13" group (XPRAM $08-$0B). */
    for (int i = 0; i < 16; i++)  rtc.pram[0x10 + i] = sysparam[i];
    for (int i = 16; i < 20; i++) rtc.pram[0x08 + i - 16] = sysparam[i];
    /* Extended PRAM: the Classic ROM's validity signature is 'Bugs' at
     * XPRAM $0C-$0F (with 'NuMc' or anything else it clears XPRAM and
     * writes 'Bugs').  $01/$02 = the values the ROM leaves after that
     * initialisation; everything else 0. */
    rtc.pram[0x0C] = 'B'; rtc.pram[0x0D] = 'u';
    rtc.pram[0x0E] = 'g'; rtc.pram[0x0F] = 's';
    rtc.pram[0x01] = 0x80;
    rtc.pram[0x02] = 0x4F;
#endif
    ESP_LOGI(TAG, "RTC initialised: seconds=%08lX SPValid=%02X SPConfig=%02X",
             (unsigned long)rtc.seconds, rtc.pram[0x10], rtc.pram[0x13]);
}

static uint8_t reg_read(uint8_t a)
{
    if (a < 8) return (uint8_t)(rtc.seconds >> (8 * (a & 3)));
    if (a == 12 || a == 13) return 0;           /* write-only */
    return rtc.pram[a];
}

static void reg_write(uint8_t a, uint8_t v)
{
    if (a == 13) { rtc.wp = (v & 0x80) != 0; return; }
    if (a == 12) return;                        /* test register */
    if (rtc.wp) return;
    if (a < 8) {
        int sh = 8 * (a & 3);
        rtc.seconds = (rtc.seconds & ~(0xFFu << sh)) | ((uint32_t)v << sh);
    } else {
        rtc.pram[a] = v;
    }
}

static void byte_in(uint8_t b)
{
    switch (rtc.state) {
    case ST_CMD:
        rtc.cmd = b;
        if ((b & 0x78) == 0x38) {               /* z0111aaa: extended */
            rtc.state = ST_XADDR;
            return;
        }
        rtc.addr = (b >> 2) & 0x1F;
        break;
    case ST_XADDR:
        rtc.addr = (uint8_t)(((rtc.cmd & 7) << 5) | ((b >> 2) & 0x1F));
        break;
    case ST_WDATA:
        if ((rtc.cmd & 0x78) == 0x38) {         /* extended write */
            macrtc_log(rtc.wp ? "XWR(wp)" : "XWR", rtc.addr, b);
            if (!rtc.wp) rtc.pram[rtc.addr] = b;
        } else {
            macrtc_log(rtc.wp && rtc.addr != 13 ? "WR(wp)" : "WR", rtc.addr, b);
            reg_write(rtc.addr, b);
        }
        rtc.state = ST_DONE;
        return;
    default:
        return;
    }
    if (rtc.cmd & 0x80) {                       /* read: chip sends next */
        rtc.out = ((rtc.cmd & 0x78) == 0x38) ? rtc.pram[rtc.addr]
                                             : reg_read(rtc.addr);
        macrtc_log((rtc.cmd & 0x78) == 0x38 ? "XRD" : "RD", rtc.addr, rtc.out);
        rtc.state = ST_SEND;
        rtc.bits = 0;
    } else {
        rtc.state = ST_WDATA;
    }
}

void macrtc_portb_changed(uint8_t orb, uint8_t ddrb)
{
    /* Undriven (input) pins read as pulled up */
    uint8_t pins = (orb & ddrb) | (uint8_t)~ddrb;
    bool en = !(pins & 0x04);
    int clk = (pins >> 1) & 1;

    if (!en) {
        rtc.en = false;
        rtc.state = ST_CMD;
        rtc.bits = 0;
        rtc.dout = 1;
        rtc.clk = clk;
        return;
    }
    if (!rtc.en) {                              /* start of a transaction */
        rtc.en = true;
        rtc.state = ST_CMD;
        rtc.bits = 0;
        rtc.shift = 0;
    }
    if (clk != rtc.clk) {
        if (clk) {                              /* rising edge */
            if (rtc.state == ST_SEND) {
                if (++rtc.bits == 8) rtc.state = ST_DONE;
            } else if (rtc.state != ST_DONE) {
                rtc.shift = (uint8_t)((rtc.shift << 1) | (pins & 1));
                if (++rtc.bits == 8) {
                    rtc.bits = 0;
                    byte_in(rtc.shift);
                }
            }
        } else if (rtc.state == ST_SEND) {      /* falling edge: next bit */
            rtc.dout = (rtc.out >> (7 - rtc.bits)) & 1;
        }
        rtc.clk = clk;
    }
}

int macrtc_data_out(void)
{
    return rtc.dout;
}

void macrtc_one_second(void)
{
    rtc.seconds++;
}

uint32_t macrtc_get_seconds(void) { return rtc.seconds; }
const uint8_t *macrtc_get_pram(void) { return rtc.pram; }

#endif /* CLASSIC_RTC && !MAC_ROM_LC_II */
