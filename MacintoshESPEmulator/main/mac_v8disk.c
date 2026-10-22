/*
 * V8 (LC II / Color Classic) floppy: high-level .Sony Prime replacement.
 * See mac_v8disk.h for the mechanism.
 */
#include "mac_v8disk.h"

#if MAC_ROM_LC_II

#include "mac_memory.h"
#include "mac_iwm.h"
#include "mac_disk_overlay.h"
#include "m68k.h"
#include <string.h>
#include <esp_log.h>

static const char *TAG = "v8disk";

#define JPRIME      0x0226u   /* .Sony Prime vector (driver Prime = MOVE.L $226,-(SP); RTS) */
#define JIODONE     0x08FCu
#define HOOK_ADDR   (V8DISK_BASE + 0x100u)

/* Mac OS error codes */
#define noErr        0
#define wPrErr     (-44)
#define paramErr   (-50)
#define nsDrvErr   (-56)
#define offLinErr  (-65)

static const uint8_t stub[] = {
    0x4A, 0x39, 0x00, 0xFE, 0x01, 0x00,   /* TST.B  $00FE0100   (hook) */
    0x2F, 0x38, 0x08, 0xFC,               /* MOVE.L $08FC.W,-(SP)      */
    0x4E, 0x75,                           /* RTS -> IODone             */
};

#define INST_OFF    0x40u    /* drive-2 install routine (MAC_DRIVE2) */


static uint32_t rom_prime;   /* the ROM driver's own Prime (logged only) */
static uint32_t n_calls;

static inline uint32_t rd32(const uint8_t *r, uint32_t a)
{
    return ((uint32_t)r[a] << 24) | ((uint32_t)r[a+1] << 16) |
           ((uint32_t)r[a+2] << 8) | r[a+3];
}
static inline void wr32(uint8_t *r, uint32_t a, uint32_t v)
{
    r[a] = v >> 24; r[a+1] = v >> 16; r[a+2] = v >> 8; r[a+3] = v;
}

#if MAC_DRIVE2
static struct {
    uint32_t size;
    v8disk_io_fn io;
    bool writable;
    bool installed;
    uint8_t code[64];
    int code_len;
} d2;

void v8disk_set_drive2(uint32_t size, v8disk_io_fn io, bool writable)
{
    d2.size = size;
    d2.io = io;
    d2.writable = writable;
    uint32_t blocks = size / 512;
    const uint8_t c[] = {
        0x20, 0x3C, 0x00, 0x00, 0x00, 0x14,         /* MOVE.L #20,D0              */
        0xA7, 0x1E,                                 /* _NewPtrSysClear            */
        0x20, 0xBC, writable ? 0x00 : 0x80, 0x08, 0x01, 0x00,
                                                    /* MOVE.L #wp/8/1/0,(A0): writeProt,
                                                       diskInPlace=8 (fixed), installed */
        0x58, 0x88,                                 /* ADDQ.L #4,A0 -> qLink      */
        0x31, 0x7C, 0x00, 0x01, 0x00, 0x04,         /* MOVE.W #1,qType(A0)        */
        0x31, 0x7C, 0xFF, 0xFB, 0x00, 0x08,         /* MOVE.W #-5,dQRefNum(A0)    */
        0x31, 0x7C, (uint8_t)(blocks >> 8), (uint8_t)blocks, 0x00, 0x0C,
                                                    /* MOVE.W #lo,dQDrvSz(A0)     */
        0x31, 0x7C, (uint8_t)(blocks >> 24), (uint8_t)(blocks >> 16), 0x00, 0x0E,
                                                    /* MOVE.W #hi,dQDrvSz2(A0)    */
        0x20, 0x3C, 0x00, 0x02, 0xFF, 0xFB,         /* MOVE.L #$0002FFFB,D0       */
        0xA0, 0x4E,                                 /* _AddDrive                  */
        0x30, 0x7C, 0x00, 0x07,                     /* MOVEA.W #diskEvt,A0        */
        0x70, 0x02,                                 /* MOVEQ #2,D0 (drive)        */
        0xA0, 0x2F,                                 /* _PostEvent                 */
        0x4E, 0x75,                                 /* RTS -> the event trap      */
    };
    memcpy(d2.code, c, sizeof c);
    d2.code_len = sizeof c;
    ESP_LOGI(TAG, "drive 2: %lu bytes (%s)", (unsigned long)size, writable ? "writable" : "locked");
}

int v8disk_aline_pre(unsigned int ir)
{
    if (!d2.size || d2.installed || (ir != 0xA860 && ir != 0xA970)) return 0;
    uint8_t *r = mac_mem_get_ram();
    if (!r || !mac_post_boot) return 0;
    uint32_t wmgr = rd32(r, 0x09DE);
    int al = r[0x0910], fl = r[0x02E0];                     /* the Finder in front */
    if (wmgr == 0 || wmgr == 0xFFFFFFFF ||
        !(al == fl && al > 0 && al < 32 && memcmp(r + 0x0911, r + 0x02E1, al) == 0))
        return 0;
    if (m68k_get_reg(NULL, M68K_REG_SR) & 0x0700) return 0;
    uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7) - 4;
    m68k_write_memory_32(sp, m68k_get_reg(NULL, M68K_REG_PPC));   /* return to the trap */
    m68k_set_reg(M68K_REG_A7, sp);
    m68k_set_reg(M68K_REG_PC, V8DISK_BASE + INST_OFF);
    d2.installed = true;
    ESP_LOGW(TAG, "drive 2: installing (AddDrive + diskEvt)");
    return 1;
}
#else
void v8disk_set_drive2(uint32_t size, v8disk_io_fn io, bool writable)
{
    (void)size; (void)io; (void)writable;
}
int v8disk_aline_pre(unsigned int ir) { (void)ir; return 0; }
#endif

void v8disk_frame(void)
{
    uint8_t *ram = mac_mem_get_ram();
    if (!ram) return;
    uint32_t jp = rd32(ram, JPRIME);
    /* Only redirect once the driver's Open has pointed JPrime into ROM. */
    if ((jp & 0xFFFFFF) >= 0xA00000 && (jp & 0xFFFFFF) < 0xB00000) {
        if (rom_prime != jp) {
            rom_prime = jp;
            ESP_LOGW(TAG, "JPrime $%08lX -> stub $%06X", (unsigned long)jp, V8DISK_BASE);
        }
        wr32(ram, JPRIME, V8DISK_BASE);
    }
}

/* .Sony Prime: A0 = IOParam, A1 = DCE.  Sets D0 for IODone. */
static void v8disk_prime(void)
{
    uint8_t *ram = mac_mem_get_ram();
    uint32_t rsz = mac_mem_get_ram_size();
    uint32_t pb  = m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF;
    uint32_t dce = m68k_get_reg(NULL, M68K_REG_A1) & 0xFFFFFF;
    int16_t err = noErr;

    if (pb + 0x32 > rsz || dce + 0x18 > rsz) {
        m68k_set_reg(M68K_REG_D0, (uint32_t)(int32_t)paramErr);
        return;
    }
    uint8_t  trap  = ram[pb + 0x07];               /* $02 _Read, $03 _Write */
    int16_t  drive = (int16_t)((ram[pb + 0x16] << 8) | ram[pb + 0x17]);
    uint32_t buf   = rd32(ram, pb + 0x20) & 0xFFFFFF;
    uint32_t cnt   = rd32(ram, pb + 0x24);
    uint32_t pos   = rd32(ram, dce + 0x10);        /* dCtlPosition */
    uint8_t *disk  = iwm_get_disk_data();
    uint32_t dsize = iwm_get_disk_size();
    uint32_t done  = 0;

    if (drive == 1 && disk && !iwm_disk_writable) {
        /* Mark the drive's DrvSts write-protected so the File Manager
         * mounts the (flash, read-only) volume locked.  The ROM driver
         * would set this in its own Prime, which is bypassed here. */
        uint32_t q = rd32(ram, 0x30A) & 0xFFFFFF;          /* DrvQHdr.qHead */
        for (int i = 0; q && q + 0x0A <= rsz && i < 16; i++) {
            if (((ram[q + 6] << 8) | ram[q + 7]) == 1 &&
                ((ram[q + 8] << 8) | ram[q + 9]) == 0xFFFB) {
                ram[q - 4] |= 0x80;                        /* writeProt */
                break;
            }
            q = rd32(ram, q) & 0xFFFFFF;
        }
    }
#if MAC_DRIVE2
    if (drive == 2 && d2.size) {
        if ((trap & 0x0F) == 0x03 && !d2.writable)
            err = wPrErr;
        else if (pos > d2.size || cnt > d2.size - pos || buf + cnt > rsz)
            err = paramErr;
        else
            err = (int16_t)d2.io((trap & 0x0F) == 0x03, ram + buf, pos, cnt);
        done = err ? 0 : cnt;
    } else
#endif
    if (drive != 1) {
        err = nsDrvErr;
    } else if (!disk || !iwm_disk_inserted()) {
        err = offLinErr;
    } else if ((trap & 0x0F) == 0x03 && !iwm_disk_writable) {
        err = wPrErr;                              /* DISK_WRITABLE=0: read-only */
    } else if (pos > dsize || cnt > dsize - pos || buf + cnt > rsz) {
        err = paramErr;
    } else if ((trap & 0x0F) == 0x03) {
        err = disk_ov_write(ram + buf, pos, cnt);  /* PSRAM overlay */
        done = err ? 0 : cnt;
    } else {
        disk_ov_read(ram + buf, pos, cnt);         /* overlay, else flash */
        done = cnt;
    }
    wr32(ram, pb + 0x28, done);                    /* ioActCount */
    wr32(ram, dce + 0x10, pos + done);             /* dCtlPosition */
    wr32(ram, pb + 0x2E, pos + done);              /* ioPosOffset */
    m68k_set_reg(M68K_REG_D0, (uint32_t)(int32_t)err);

    static uint32_t n_logw;                        /* writes / errors logged */
    if (n_calls < 40 || ((err != noErr || (trap & 0x0F) == 0x03) && n_logw++ < 60)) {
        ESP_LOGW(TAG, "PRIME #%lu %s drv=%d pos=%lu cnt=%lu buf=$%06lX err=%d",
                 (unsigned long)n_calls, (trap & 0x0F) == 0x03 ? "W" : "R",
                 drive, (unsigned long)pos, (unsigned long)cnt,
                 (unsigned long)buf, err);
    }
    n_calls++;
}

uint8_t v8disk_read(uint32_t address)
{
    uint32_t off = address - V8DISK_BASE;
    if (off < sizeof(stub)) return stub[off];
#if MAC_DRIVE2
    if (off >= INST_OFF && off < INST_OFF + (uint32_t)d2.code_len) return d2.code[off - INST_OFF];
#endif
    if (address == HOOK_ADDR) {
        v8disk_prime();
        return 0;
    }
    return 0xFF;
}

#endif /* MAC_ROM_LC_II */
