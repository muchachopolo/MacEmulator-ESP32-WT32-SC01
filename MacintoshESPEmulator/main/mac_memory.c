/*
 * Macintosh Memory Map and Musashi CPU Callbacks
 *
 * This file implements the 68000 memory bus for the Mac Classic.
 * All m68k_read/write_memory_* functions required by Musashi are defined here.
 */
#include "mac_memory.h"
#include "mac_emulator.h"
#include "mac_via.h"
#include "mac_scc.h"
#include "mac_scsi.h"
#include "mac_iwm.h"
#include "mac_v8video.h"
#include "mac_v8disk.h"
#include "mac_disk_overlay.h"   /* writable disk (PSRAM overlay) */
#include "m68k.h"

#include <string.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <esp_system.h>   /* esp_restart(): Classic Restart */

static const char *TAG = "mac_mem";
int initgraf_seen = 0;  /* Set when _InitGraf ROM entry is accessed */

/* Memory arrays */
static uint8_t *ram = NULL;         /* Main RAM (in PSRAM) */
static const uint8_t *rom = NULL;   /* ROM image (in flash) */
static uint32_t ram_size = 0;
static uint32_t rom_size = 0;
static uint32_t rom_mask = 0;       /* For mirroring: rom_size - 1 */
static uint32_t ram_mask = 0;       /* For mirroring: ram_size - 1 */


/* ESP32 A/B switch (default 0): put the data-access callbacks
 * m68k_read/write_memory_8/16/32 in IRAM like the fetch callbacks below.
 * With M68K_INLINE_RAM they only serve ROM data, devices, low-memory writes
 * and alias/edge cases; ~2KB of IRAM. */
#ifndef MAC_MEM_CALLBACK_IRAM
#define MAC_MEM_CALLBACK_IRAM 1
#endif
#if MAC_MEM_CALLBACK_IRAM
#define MEM_CB_ATTR IRAM_ATTR
#else
#define MEM_CB_ATTR
#endif

/* Boot bypass flag — set by mac_emulator.c when disk is ready */
volatile bool mac_boot_read_bypass = false;

/* Post-boot mode — set after Finder is running. Enables lean instruction
 * fetch path that skips diagnostic watchpoints and logging intercepts. */
bool mac_post_boot = false;

/* Inline RAM fast-path limits read by Musashi (m68kconf.h M68K_INLINE_RAM /
 * M68K_INLINE_FETCH). 0 = disabled: every access takes the callbacks below.
 * Each callback's own RAM fast path arms its limit (idempotent store) the
 * first time it runs with its preconditions met (overlay off; post-boot for
 * fetches). Bounds stop 3 bytes short of ram_size so a 32-bit access never
 * crosses the end (the callbacks keep the Classic alias-wrap handling), and
 * writes below $0A20 always take the callback (boot-time guards). With
 * MAC_DIAG the limits stay 0 so every diagnostic still sees every access. */
unsigned char *mac_fast_ram;
unsigned int   mac_fast_rd_lim;
unsigned int   mac_fast_wr_span;
unsigned int   mac_fast_fetch_lim;
#if MAC_DIAG
#define ARM_FAST_RD()    do { } while (0)
#define ARM_FAST_WR()    do { } while (0)
#define ARM_FAST_FETCH() do { } while (0)
#else
#define ARM_FAST_RD()    (mac_fast_rd_lim = ram_size - 3)
#define ARM_FAST_WR()    (mac_fast_wr_span = ram_size - 3 - 0x0A20)
#define ARM_FAST_FETCH() (mac_fast_fetch_lim = ram_size - 3)
#endif

#if MAC_ROM_LC_II
/* VRAM fast path: $F40000-$FBFFFF straight to the V8 VRAM buffer (PSRAM).
 * Checked only AFTER the RAM/ROM fast paths so their cost is unchanged.
 * (address - base) < size is one unsigned compare. */
static uint8_t *v8_vram;
/* Native-LCD declaration-ROM copy (byte reads only, see mac_v8video.c) */
static const uint8_t *v8_decl;
static uint32_t v8_decl_off = 0xFFFFFFFF;
#ifndef V8_NO_VRAM_FASTPATH
#define VRAM_HIT(a, n)  ((uint32_t)((a) - V8_VRAM_BASE) <= (uint32_t)(V8_VRAM_SIZE - (n)))
#else
#define VRAM_HIT(a, n)  0   /* A/B benchmarking only */
#endif
uint32_t v8_fetch_ring[256];
int v8_fetch_idx;
#endif

/* ROM-variant address table. The LC II ROM is the same OS source compiled
 * for 68020: every routine is a structural clone of the Classic ROM's at a
 * different address. Intercepts are pinned per-ROM. */
#if MAC_ROM_LC_II
#define BOOT_READ_HELPER 0x401616   /* boot block _Read helper (sig check $4C4B) */
#define DM_READ_ENTRY    0x40BB24   /* DM _Read handler entry */
#define DM_SPIN_LOOP     0x40BB8C   /* DM _Read ioResult>0 polling loop */
#define DM_CTL_ENTRY     0x40BBAE   /* DM _Control handler entry */
#define DM_STAT_ENTRY    0x40BBBE   /* DM _Status handler entry */
#else
#define BOOT_READ_HELPER 0x400E0A
#define DM_READ_ENTRY    0x40315C
#define DM_SPIN_LOOP     0x4031BE
#define DM_CTL_ENTRY     0x4031E0
#define DM_STAT_ENTRY    0x4031EE
#define DM_WRITE_ENTRY   0x4031CC   /* DM _Write handler entry (writable disk) */
#endif

#if !MAC_ROM_LC_II && CLASSIC_NATIVE_LCD
/* Classic native 480x320 ROM patches (CLASSIC_NATIVE_LCD, see mac_emulator.h).
 * Each entry is the immediate word of a MOVE.W #imm (ROM file offsets):
 *   $005F6 InitCrsr globals: MOVE.W #342,(A0)+ / MOVE.W #512,(A0) -> CrsrPin
 *          ($0834) botRight.         imm words at $0060A / $0060E
 *   $18F94 ScrnSize (JScrnSize $0810): MOVE.W #342,(A0) / MOVE.W #512,(A0)
 *                                     imm words at $18F9A / $18FA0
 *   $18FA4 _ScrnBitMap (called by InitGraf for screenBits): baseAddr =
 *          ScrnBase, rowBytes = ScreenRow, bounds 0,0,#342 then BRA into
 *          ScrnSize's #512.          imm word  at $18FB4
 * rowBytes (ScreenRow $0106 = 64) and ScrnBase are unchanged.
 *
 * Boot screens that do not use those values (hard-wired for 512x342):
 *   Happy Mac / disk icons: the boot code draws straight into the frame
 *   buffer at absolute addresses (MOVEA.L #imm,A2), 32x32 cell at x 240,
 *   y 145 = centred on 512 and 10 px above centre on 342. Moved by -$82
 *   to x 224, y 143 (centred on 480x320; the LC II ROM uses y 144). The
 *   cell carries the 50% grey desktop pattern in its margins, so the row
 *   must keep its parity (odd), and x must stay byte-aligned:
 *     $00F4A disk icon   #$3FCB5E -> #$3FCADC (long at $00F4C)
 *     $00F5C disk symbol #$3FCF1F -> #$3FCE9D (long at $00F5E)
 *     $01188 Mac outline #$3FCB5E -> #$3FCADC (long at $0118A)
 *     $01196 Mac face    #$3FCCDF -> #$3FCC5D (long at $01198)
 *   SysError / DSAlert box (System 7 "Welcome to Macintosh", progress bar):
 *   the default DSAlertRect ($03F8, global) is the constant 64,32,190,480,
 *   centred on 512; the DSAT items (icon, text, buttons) are drawn at
 *   absolute local coordinates made for that rect, and the System's
 *   extension progress bar is placed from DSAlertRect. The default path
 *   ($4015 5A-7D) is rewritten in place to store 63,16,189,464 (global),
 *   draw it, set bit 7 as before, then MovePortTo(-16,-1) so the DSAT items
 *   follow the box (the else path already does MovePortTo(rect.topLeft)):
 *     $0155A LEA ($03F8).W,A0              (unchanged)
 *     $0155E TST.B (A0) / BPL else         (was BTST #7,($03F8).W / BEQ)
 *     $01562 MOVEA.L A0,A2                 (new entry, see $01662)
 *     $01564 MOVE.L #$003F0010,(A0)
 *     $0156A MOVE.L #$00BD01D0,4(A0)
 *     $01572 BSR $401588 (draw)            $01574 BSET #7,(A2)
 *     $01578 PEA ($FFF0).W  -> (v -1, h -16) $0157C BRA _MovePortTo;RTS
 *     $0157E else path                     (unchanged)
 *   The alert-$0D path ($0165A) BSRs to the old store entry $401566 and
 *   then does MovePortTo(38,70): BSR -> $401562, MovePortTo -> (22,69).
 * The 32-bit immediates are fetched through m68k_read_immediate_32, so they
 * are patched there (classic_native_long); the 16-bit table below carries
 * the same words for consistency. */
#define CL_NAT_LO  0x18F9A   /* lean-path window [$18F9A, $18FB4] */
#define CL_NAT_SPAN (0x18FB4 - 0x18F9A)
#define CL_DS_LO   0x0155E   /* DSAlert window [$155E, $1669] */
#define CL_DS_SPAN (0x01669 - 0x0155E)
static inline uint16_t classic_native_word(uint32_t off, uint16_t w)
{
    switch (off) {
    case 0x0060A: case 0x18F9A: case 0x18FB4: return 0x0140;   /* 320 */
    case 0x0060E: case 0x18FA0:               return 0x01E0;   /* 480 */
    /* DSAlert default path rewrite (see above) */
    case 0x0155E: return 0x4A10;   /* TST.B (A0)            */
    case 0x01560: return 0x6A1C;   /* BPL $40157E           */
    case 0x01562: return 0x2448;   /* MOVEA.L A0,A2         */
    case 0x01564: return 0x20BC;   /* MOVE.L #,(A0)         */
    case 0x01566: return 0x003F;
    case 0x01568: return 0x0010;
    case 0x0156A: return 0x217C;   /* MOVE.L #,4(A0)        */
    case 0x0156C: return 0x00BD;
    case 0x0156E: return 0x01D0;
    case 0x01570: return 0x0004;
    case 0x01572: return 0x6114;   /* BSR $401588           */
    case 0x01574: return 0x08D2;   /* BSET #7,(A2)          */
    case 0x01576: return 0x0007;
    case 0x01578: return 0x4878;   /* PEA ($FFF0).W         */
    case 0x0157A: return 0xFFF0;
    case 0x0157C: return 0x6006;   /* BRA $401584           */
    case 0x01662: return 0xFF00;   /* alert $0D: BSR $401562 */
    case 0x01666: return 0x0045;   /* MovePortTo(22,69)     */
    case 0x01668: return 0x0016;
    }
    return w;
}
/* 32-bit immediates (offset of the long). Window [$0F4C, $1666]. */
#define CL_NAT32_LO   0x00F4C
#define CL_NAT32_SPAN (0x01666 - 0x00F4C)
static inline uint32_t classic_native_long(uint32_t off, uint32_t v)
{
    switch (off) {
    case 0x00F4C: case 0x0118A: return 0x003FCADC;
    case 0x00F5E:               return 0x003FCE9D;
    case 0x01198:               return 0x003FCC5D;
    case 0x01566:               return 0x003F0010;
    case 0x0156C:               return 0x00BD01D0;
    case 0x01666:               return 0x00450016;
    }
    return v;
}
#define CLASSIC_NATIVE_PATCH32(off, v) \
    do { if (__builtin_expect((uint32_t)((off) - CL_NAT32_LO) <= CL_NAT32_SPAN, 0)) \
             (v) = classic_native_long((off), (v)); } while (0)
#define CLASSIC_NATIVE_PATCH(off, w) \
    do { if (__builtin_expect((uint32_t)((off) - CL_NAT_LO) <= CL_NAT_SPAN || (off) < 0x610 || \
                              (uint32_t)((off) - CL_DS_LO) <= CL_DS_SPAN, 0)) \
             (w) = classic_native_word((off), (w)); } while (0)
#define CLASSIC_NATIVE_PATCH_LEAN(off, w) \
    do { if (__builtin_expect((uint32_t)((off) - CL_NAT_LO) <= CL_NAT_SPAN || \
                              (uint32_t)((off) - CL_DS_LO) <= CL_DS_SPAN, 0)) \
             (w) = classic_native_word((off), (w)); } while (0)
#else
#define CLASSIC_NATIVE_PATCH(off, w)      do { } while (0)
#define CLASSIC_NATIVE_PATCH_LEAN(off, w) do { } while (0)
#define CLASSIC_NATIVE_PATCH32(off, v)    do { } while (0)
#endif

/* Byte-read helpers for big-endian data */
#define READ_BYTE(base, addr)   ((base)[addr])
#define READ_WORD(base, addr)   (((base)[addr] << 8) | (base)[(addr) + 1])
#define READ_LONG(base, addr)   (((base)[addr] << 24) | ((base)[(addr)+1] << 16) | \
                                 ((base)[(addr)+2] << 8) | (base)[(addr)+3])

#define WRITE_BYTE(base, addr, val)  (base)[addr] = (val) & 0xFF
#define WRITE_WORD(base, addr, val)  do { \
    (base)[addr]     = ((val) >> 8) & 0xFF; \
    (base)[(addr)+1] = (val) & 0xFF; \
} while(0)
#define WRITE_LONG(base, addr, val)  do { \
    (base)[addr]     = ((val) >> 24) & 0xFF; \
    (base)[(addr)+1] = ((val) >> 16) & 0xFF; \
    (base)[(addr)+2] = ((val) >> 8) & 0xFF; \
    (base)[(addr)+3] = (val) & 0xFF; \
} while(0)

bool mac_mem_init(const uint8_t *rom_data, uint32_t rom_sz,
                  uint8_t *disk_data, uint32_t disk_size)
{
    rom = rom_data;
    rom_size = rom_sz;
    rom_mask = rom_size - 1;

    ram_size = MAC_RAM_SIZE;
    ram_mask = ram_size - 1;

    /* Allocate RAM in PSRAM */
    ram = (uint8_t *)heap_caps_calloc(1, ram_size, MALLOC_CAP_SPIRAM);
    if (!ram) {
        ESP_LOGE(TAG, "Failed to allocate %lu bytes of RAM in PSRAM", (unsigned long)ram_size);
        /* Fallback: try smaller RAM */
        ram_size = 1 * 1024 * 1024;  /* 1MB */
        ram_mask = ram_size - 1;
        ram = (uint8_t *)heap_caps_calloc(1, ram_size, MALLOC_CAP_SPIRAM);
        if (!ram) {
            ESP_LOGE(TAG, "Failed to allocate even 1MB of RAM");
            return false;
        }
        ESP_LOGW(TAG, "Reduced RAM to 1MB");
    }

    mac_fast_ram = ram;

    ESP_LOGI(TAG, "Memory initialized: %luKB RAM (PSRAM), %luKB ROM (flash)",
             (unsigned long)(ram_size / 1024), (unsigned long)(rom_size / 1024));

    /* Initialize peripherals */
    via_init();
    scc_init();
    scsi_init();
    iwm_init(disk_data, disk_size);
#if MAC_ROM_LC_II
    if (!v8video_init()) return false;
    v8_vram = v8video_get_vram();
    v8_decl_off = v8video_patch_rom(rom, rom_size, &v8_decl);
#endif

    return true;
}

uint8_t *mac_mem_get_ram(void)
{
    return ram;
}

uint32_t mac_mem_get_ram_size(void)
{
    return ram_size;
}

void mac_mem_sync_fast_ram(void)
{
    /* No-op: fast_ram shadow was removed (overhead exceeded benefit). */
}

/* ====================================================================== */
/* ================ MEMORY ACCESS DISPATCH (address decode) ============= */
/* ====================================================================== */

static inline uint8_t mem_read_byte(uint32_t address)
{
    /* Mask to 24-bit address bus */
    address &= 0x00FFFFFF;

#if MAC_ROM_LC_II
#if MAC_DIAG
    static uint32_t spin_addr, spin_pc, spin_rep;
    static int spin_logged;
    if (address == spin_addr) {
        uint32_t pcv = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
        if (pcv == spin_pc) {
            if (spin_rep++ == 3000 && spin_logged < 8) {
                ESP_LOGW(TAG, "V8SPIN #%d: addr=$%06X PC=$%06X",
                         spin_logged++, address, pcv);
            }
        } else {
            spin_pc = pcv;
            spin_rep = 0;
        }
    } else {
        spin_addr = address;
        spin_pc = 0;
        spin_rep = 0;
    }
    static int v8trace = 0;
    if (v8trace < 60 &&
        ((address < 0x40 && via_get_overlay()) ||
         (address >= 0x5FFFD2 && address < 0x5FFFF2))) {
        ESP_LOGW(TAG, "V8BOOT #%d: $%06X -> %02X PC=$%06X ov=%d",
                 v8trace++, address,
                 (address >= 0x5FFFD2) ? rom[0x0FFD2 + (address - 0x5FFFD2)]
                                       : rom[address & rom_mask],
                 m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF,
                 via_get_overlay());
    }
#endif /* MAC_DIAG */
    /* V8 (Color Classic / LC II): ROM $40A00000-$40AFFFFF aliases to the
     * $A00000-$AFFFFF 24-bit window (A24-A30 undecoded); RAM $0-$9FFFFF;
     * devices overlay RAM inside their windows. Overlay mirrors ROM at
     * $0-$7FFFF only — RAM never moves to $600000 on V8. */
    if (via_get_overlay()) {
        if (address < 0x400 && via_get_overlay_was_off()) {
            return ram[address];
        }
        if (address < 0x80000) {
            return rom[address & rom_mask];
        }
    }
    if (address >= 0xA00000 && address < 0xB00000) {
        if (via_overlay_fast) {
            ESP_LOGI(TAG, "V8 overlay cleared by ROM access $%06X PC=$%06X", address,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
            via_clear_overlay();
        }
        uint32_t ro = address & rom_mask;
        if (ro >= v8_decl_off) return v8_decl[ro - v8_decl_off];
        return rom[ro];
    }
    /* Device bases from the ROM's own decoder table (file $3490):
     * VIA1 $50F00000, SWIM $50F16000; $50FC0000 is the diag-ROM probe. */
    if (address >= 0xF00000 && address < 0xF02000) {
        return via_read(address);
    }
    if (address >= 0xF16000 && address < 0xF18000) {
        return iwm_read(address);
    }
    /* SCC $50F04000 (same A1=channel, A2=data decode as the Classic) and
     * NCR 5380 SCSI $50F10000 regs (A6-A4 = register) + $50F12000 pDMA. */
    if (address >= 0xF04000 && address < 0xF06000) {
        return scc_read(address);
    }
    if (address >= 0xF10000 && address < 0xF14000) {
        return scsi_read(address);
    }
    /* VDAC $F24000, pseudo-VIA2 $F26000, VRAM $F40000-$FBFFFF */
    if (address >= 0xF24000 && address < 0xFC0000 &&
        (address >= V8_VRAM_BASE || address < 0xF28000)) {
        return v8video_read(address);
    }
    /* V8 RAM does not alias: $ram_size-$9FFFFF is unpopulated (open bus),
     * so the ROM's sizer at $AA01D8 finds exactly ram_size bytes. */
    if (address < ram_size) {
        return ram[address];
    }
    /* --- v8disk hook: .Sony Prime stub + trigger (mac_v8disk.c) --- */
    if (address >= V8DISK_BASE && address < V8DISK_END) {
        return v8disk_read(address);
    }
    /* --- end v8disk hook --- */
    return 0xFF;
#else
    if (via_get_overlay()) {
        /* Overlay mode: ROM at 0x000000, RAM at 0x600000.
         * Exception: once first-stage boot has turned overlay OFF (copying
         * real exception vectors to RAM), subsequent overlay-ON periods
         * must read the vector table (0x0000-0x03FF) from RAM. Otherwise
         * exception lookups hit ROM boot code instead of installed handlers. */
        if (address < 0x400 && via_get_overlay_was_off()) {
            return ram[address];
        }
        if (address < 0x400000) {
            return rom[address & rom_mask];
        }
        if (address >= 0x400000 && address < 0x500000) {
            return rom[address & rom_mask];
        }
        if (address >= 0x600000 && address < 0x800000) {
            return ram[(address - 0x600000) & ram_mask];
        }
    } else {
        /* Normal mode: RAM at 0x000000, ROM at 0x400000.
         * Addresses beyond physical RAM alias via ram_mask (power-of-2),
         * matching real Mac Classic hardware behavior. */
        if (address < 0x400000) {
            return ram[address & ram_mask];
        }
        if (address >= 0x400000 && address < 0x500000) {
            return rom[address & rom_mask];
        }
    }
#endif

    /* SCSI NCR 5380: 0x580000-0x5FFFFF */
    if (address >= 0x580000 && address < 0x600000) {
        return scsi_read(address);
    }

    /* SCC read */
    if (address >= 0x800000 && address < 0xA00000) {
        return scc_read(address);
    }

    /* SCC write space (reads return open bus) */
    if (address >= 0xA00000 && address < 0xC00000) {
        return 0xFF;
    }

    /* IWM/SWIM */
    if (address >= 0xC00000 && address < 0xE00000) {
        return iwm_read(address);
    }

    /* VIA / SWIM shared space ($E00000-$EFFFFF)
     * On the Mac Classic, both VIA and SWIM share this address range.
     * The ROM stores $EFE1FE at $01D4 and the SWIM driver loads A2
     * from $01D4 to access ISM registers at A2+$1000 (DATA), A2+$1200
     * (MARK), etc.  On real hardware, the Gemini chip routes these to
     * SWIM when ISM mode is active.  We only route to IWM when the
     * current PC is in the SWIM driver ROM ($434000-$436000) AND the
     * offset has A12=1, so VIA timer/interrupt registers remain
     * accessible to all other code (interrupt handlers, init, etc.). */
    if (address >= 0xE00000 && address < 0xF00000) {
        if ((address & 0x1000)) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            if (pc >= 0x434000 && pc < 0x436000) {
                return iwm_read(address);
            }
        }
        return via_read(address);
    }

    /* Unmapped space */
    return 0xFF;
}

static inline void mem_write_byte(uint32_t address, uint8_t value)
{
    address &= 0x00FFFFFF;

    /* GUARD: Protect only our safe handler at $0030-$0039.
     * The heap corruption root cause (DrvQEl at $1700 inside the
     * system heap's master pointer chain) is fixed — DrvQEl now
     * lives at $0040-$0057.  The broad $0000-$003F guard was
     * blocking legitimate ROM updates to the Line-A vector ($0028)
     * which is required for QuickDraw.  Now we only protect the
     * 10 bytes of our safe handler + recovery loop. */
    if (mac_boot_read_bypass && address >= 0x0030 && address <= 0x0039) {
        static int vec_wp = 0;
        if (vec_wp < 10) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            ESP_LOGW(TAG, "!VEC-GUARD: $%04X<-%02X PC=$%06X SP=$%06X",
                     address, value, pc,
                     m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
            vec_wp++;
        }
        return;  /* Block the write — preserve safe handler */
    }

#if MAC_DIAG
    /* LOG: Monitor BootDrive ($0210-$0211) writes.
     * Previously guarded, but blocking may prevent ptch resources from
     * completing.  Now just logging. */
    if (mac_boot_read_bypass && address >= 0x0210 && address <= 0x0211) {
        static int bd_cnt = 0;
        if (bd_cnt < 10) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            ESP_LOGW(TAG, "BD-WRITE: $%04X<-%02X PC=$%06X SP=$%06X",
                     address, value, pc,
                     m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
            bd_cnt++;
        }
        /* Allow the write through */
    }

    /* Watchpoint: catch writes to MemTop ($0108-$010B) after F600.
     * The ROM sets MemTop correctly, but something corrupts it during
     * boot block/System file loading. */
    if (mac_boot_read_bypass && address >= 0x0108 && address <= 0x010B) {
        static int wp_cnt = 0;
        if (wp_cnt < 10) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            ESP_LOGW(TAG, "WP-MEMTOP: $%04X<-%02X PC=$%06X SP=$%06X",
                     address, value, pc,
                     m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
            wp_cnt++;
        }
    }

    /* Watchpoint: catch writes to ScrnBase ($0824-$0827). */
    if (mac_boot_read_bypass && address >= 0x0824 && address <= 0x0827) {
        static int wp2_cnt = 0;
        if (wp2_cnt < 10) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            ESP_LOGW(TAG, "WP-SCRNBASE: $%04X<-%02X PC=$%06X SP=$%06X",
                     address, value, pc,
                     m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
            wp2_cnt++;
        }
    }

    /* Watchpoint: catch writes to WMgrPort ($09DE-$09E1).
     * WMgrPort=$FFFFFFFF at F2000+ = Window Manager never initialized.
     * Need to know WHO sets it to $FFFFFFFF and whether _InitWindows runs. */
    if (mac_boot_read_bypass && address >= 0x09DE && address <= 0x09E1) {
        static int wmgr_cnt = 0;
        if (wmgr_cnt < 20) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            ESP_LOGW(TAG, "WP-WMGR: $%04X<-%02X PC=$%06X SP=$%06X",
                     address, value, pc,
                     m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
            wmgr_cnt++;
        }
    }

    /* Smart watchpoint: log COMPLETE ApplZone value when last byte written.
     * Also dump SysZone bkLim to understand zone relationship. */
    if (mac_boot_read_bypass && address == 0x02AD) {
        static int az_cnt = 0;
        if (az_cnt < 30) {
            /* Read the full 4-byte value that's being formed */
            uint32_t new_az = ((uint32_t)ram[0x2AA]<<24)|
                              ((uint32_t)ram[0x2AB]<<16)|
                              ((uint32_t)ram[0x2AC]<<8)|value;
            uint32_t sz_addr = ((uint32_t)ram[0x2A6]<<24)|
                               ((uint32_t)ram[0x2A7]<<16)|
                               ((uint32_t)ram[0x2A8]<<8)|ram[0x2A9];
            uint32_t sz = sz_addr & 0xFFFFFF;
            uint32_t sz_bklim = 0;
            if (sz + 4 <= ram_size) {
                sz_bklim = ((uint32_t)ram[sz]<<24)|((uint32_t)ram[sz+1]<<16)|
                           ((uint32_t)ram[sz+2]<<8)|ram[sz+3];
            }
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            ESP_LOGW(TAG, "WP-AZ #%d: ApplZone=$%08lX SysZone=$%06X bkLim=$%08lX PC=$%06X SP=$%06X",
                     az_cnt, (unsigned long)new_az, sz,
                     (unsigned long)sz_bklim, pc,
                     m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
            az_cnt++;
        }
    }

    /* Watchpoint: catch writes to SysZone bkLim ($1600-$1603). */
    if (mac_boot_read_bypass && address >= 0x1600 && address <= 0x1603) {
        static int bk_cnt = 0;
        if (bk_cnt < 20) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            ESP_LOGW(TAG, "WP-BKLIM: $%04X<-%02X PC=$%06X SP=$%06X",
                     address, value, pc,
                     m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
            bk_cnt++;
        }
    }

    /* Watchpoint: catch writes to SysZone allocPtr ($162E-$1631). */
    if (mac_boot_read_bypass && address >= 0x162E && address <= 0x1631) {
        static int ap_cnt = 0;
        if (ap_cnt < 20) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            ESP_LOGW(TAG, "WP-ALLOCPTR: $%04X<-%02X PC=$%06X SP=$%06X",
                     address, value, pc,
                     m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
            ap_cnt++;
        }
    }

    /* Watchpoint: HeapEnd ($0114-$0117) — log complete value on last byte. */
    if (mac_boot_read_bypass && address == 0x0117) {
        static int he_cnt = 0;
        if (he_cnt < 20) {
            uint32_t new_he = ((uint32_t)ram[0x114]<<24)|
                              ((uint32_t)ram[0x115]<<16)|
                              ((uint32_t)ram[0x116]<<8)|value;
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            ESP_LOGW(TAG, "WP-HE: HeapEnd=$%08lX PC=$%06X SP=$%06X",
                     (unsigned long)new_he, pc,
                     m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
            he_cnt++;
        }
    }

    /* Watchpoint: catch writes to GrayRgn ($0A1C-$0A1F).
     * GrayRgn=$00000000 means desktop region never created. */
    if (mac_boot_read_bypass && address >= 0x0A1C && address <= 0x0A1F) {
        static int gray_cnt = 0;
        if (gray_cnt < 20) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            ESP_LOGW(TAG, "WP-GRAY: $%04X<-%02X PC=$%06X SP=$%06X",
                     address, value, pc,
                     m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
            gray_cnt++;
        }
    }

    /* DEBUG: Watchpoint on MBState ($0172) to detect Mac-side overwrites */
    if (mac_boot_read_bypass && address == 0x0172) {
        static int mb_cnt = 0;
        if (mb_cnt < 30) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            ESP_LOGW(TAG, "WP-MBSTATE: $0172<-%02X PC=$%06X SP=$%06X",
                     value, pc,
                     m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
            mb_cnt++;
        }
    }

#endif /* MAC_DIAG */
    /* Drive queue guard: ROM at $402B26 zeroes DrvQHdr ($0308-$0311)
     * before every boot block execution.  Catch the last byte ($0311)
     * and immediately re-install qHead/qTail -> $000046.
     * The DrvQEl data at $0040-$0057 is in reserved 68000 vectors
     * that the CPU never generates — safe from heap corruption. */
    if (mac_boot_read_bypass && address == 0x0311 && value == 0) {
        uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
        if (pc >= 0x402B20 && pc <= 0x402B40) {
            /* Re-install qHead=$000046, qTail=$000046 directly in RAM */
            ram[0x030A]=0x00; ram[0x030B]=0x00;
            ram[0x030C]=0x00; ram[0x030D]=0x46;
            ram[0x030E]=0x00; ram[0x030F]=0x00;
            ram[0x0310]=0x00; ram[0x0311]=0x46;
            static int guard_count = 0;
            if (guard_count < 5) {
                ESP_LOGW(TAG, "DRVQ-GUARD: re-installed qHead=$0046 (ROM cleared at PC=$%06X)", pc);
                guard_count++;
            }
            return; /* Don't write the 0 — we just wrote $0046 */
        }
    }

#if MAC_ROM_LC_II
    /* V8 writes: ROM window $A00000-$AFFFFF is read-only on the bus. */
    if (address >= 0xA00000 && address < 0xC00000) {
        return;
    }
    if (address >= 0xF00000 && address < 0xF02000) {
        via_write(address, value);
        return;
    }
    if (address >= 0xF16000 && address < 0xF18000) {
        iwm_write(address, value);
        return;
    }
    if (address >= 0xF04000 && address < 0xF06000) {
        scc_write(address, value);
        return;
    }
    if (address >= 0xF10000 && address < 0xF14000) {
        scsi_write(address, value);
        return;
    }
    if (address >= 0xF24000 && address < 0xFC0000 &&
        (address >= V8_VRAM_BASE || address < 0xF28000)) {
        v8video_write(address, value);
        return;
    }
#if MAC_DIAG
    static int v8w = 0;
    if (v8w < 10 && (address == 0x2002A0 || address == 0x1002F8 ||
                     (address >= 0x1FFAC0 && address <= 0x1FFAF0))) {
        ESP_LOGW(TAG, "V8WTEST #%d: $%06X<-%02X PC=$%06X",
                 v8w++, address, value,
                 m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
    }
#endif /* MAC_DIAG */
    if (address < ram_size) {
        ram[address] = value;
    }
    return;
#else
    if (via_get_overlay()) {
        /* Overlay mode: ROM at 0x000000 for READS, but writes always go to RAM
         * (ROM is physically read-only — overlay only controls the ROM chip
         * select for reads, not the RAM chip for writes).
         * Aliasing via ram_mask matches real hardware. */
        if (address < 0x400000) {
            ram[address & ram_mask] = value;
            return;
        }
        if (address >= 0x600000 && address < 0x800000) {
            ram[(address - 0x600000) & ram_mask] = value;
            return;
        }
    } else {
        /* Normal mode: RAM at 0x000000, ROM at 0x400000.
         * Alias via ram_mask like real hardware. */
        if (address < 0x400000) {
            ram[address & ram_mask] = value;
            return;
        }
        /* ROM at 0x400000-0x4FFFFF — fall through to peripheral checks */
    }

    /* SCSI NCR 5380: 0x580000-0x5FFFFF */
    if (address >= 0x580000 && address < 0x600000) {
        scsi_write(address, value);
        return;
    }

    /* Writes below peripheral space: ROM or unmapped */
    if (address < 0x800000) {
        return; /* Write to ROM/unmapped - ignored */
    }

    /* SCC write */
    if (address >= 0xA00000 && address < 0xC00000) {
        scc_write(address, value);
        return;
    }

    /* IWM/SWIM */
    if (address >= 0xC00000 && address < 0xE00000) {
        iwm_write(address, value);
        return;
    }

    /* VIA / SWIM shared space — see read handler for routing logic */
    if (address >= 0xE00000 && address < 0xF00000) {
        if ((address & 0x1000)) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            if (pc >= 0x434000 && pc < 0x436000) {
                iwm_write(address, value);
                return;
            }
        }
        via_write(address, value);
        return;
    }
#endif

    /* Writes to unmapped space are silently ignored */
}

/* ====================================================================== */
/* =================== MUSASHI REQUIRED CALLBACKS ======================= */
/* ====================================================================== */

MEM_CB_ATTR unsigned int m68k_read_memory_8(unsigned int address)
{
    /* Fast path: normal mode RAM read (most common case).
     * Alias via ram_mask — matches real Mac hardware behavior. */
    address &= 0x00FFFFFF;
#if MAC_ROM_LC_II
    if (__builtin_expect(!via_overlay_fast &&
        (address < ram_size), 1)) {
        ARM_FAST_RD();
        return ram[address & ram_mask];
    }
    if (VRAM_HIT(address, 1)) return v8_vram[address - V8_VRAM_BASE];
#else
    if (__builtin_expect(!via_overlay_fast && address < 0x400000, 1)) {
        ARM_FAST_RD();
        return ram[address & ram_mask];
    }
#endif
    return mem_read_byte(address);
}

/* ====================================================================== */
/* =========== ROM READ INTERCEPT HELPERS (for m68k_read_memory_16) ===== */
/* ====================================================================== */

/* Helper: read a 16-bit word from ROM at a given 24-bit address */
static inline uint16_t rom_word_at(uint32_t addr)
{
    uint32_t a = addr & rom_mask;
    return (rom[a] << 8) | rom[a + 1];
}

/* DM-spin rescue: intercept at $4031BE (DM _Read polling loop).
 * When the CPU fetches the loop instruction, check if A0's PB is
 * a .SONY request with ioResult > 0 (stuck).  Force-complete it.
 * Returns the real ROM word if not a stuck .SONY request. */
static uint16_t read16_dm_spin_rescue(void)
{
    uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF;
    uint32_t a0m = a0 & ram_mask;
    if (ram && a0m + 0x32 < ram_size) {
        int16_t ioRefNum = (int16_t)((ram[a0m+0x18]<<8)|ram[a0m+0x19]);
        int16_t ioResult = (int16_t)((ram[a0m+0x10]<<8)|ram[a0m+0x11]);
        if (ioRefNum == -5 && ioResult > 0) {
            /* Fulfill from disk image */
            uint32_t io_buf = READ_LONG(ram, a0m+0x20);
            uint32_t io_cnt = READ_LONG(ram, a0m+0x24);
            int32_t  io_off = (int32_t)READ_LONG(ram, a0m+0x2E);
            uint8_t *disk = iwm_get_disk_data();
            uint32_t dsize = iwm_get_disk_size();
            uint32_t buf_m = io_buf & ram_mask;
            if (disk && io_cnt > 0 && io_cnt <= 0x100000 &&
                buf_m + io_cnt <= ram_size &&
                io_off >= 0 && (uint32_t)io_off + io_cnt <= dsize) {
                disk_ov_xfer(ram[a0m+0x07] == 0x03, ram + buf_m, io_off, io_cnt); /* ioTrap $A003: write */
            }
            /* ioResult = noErr, ioActCount = ioReqCount */
            ram[a0m+0x10] = 0; ram[a0m+0x11] = 0;
            ram[a0m+0x28]=ram[a0m+0x24]; ram[a0m+0x29]=ram[a0m+0x25];
            ram[a0m+0x2A]=ram[a0m+0x26]; ram[a0m+0x2B]=ram[a0m+0x27];
            m68k_set_reg(M68K_REG_D0, 0);
            static int rescue_cnt = 0;
            if (rescue_cnt < 30) {
                ESP_LOGW(TAG, "DM-RESCUE #%d PB=$%06X off=%ld cnt=%lu",
                         rescue_cnt, a0, (long)io_off, (unsigned long)io_cnt);
                rescue_cnt++;
            }
        }
    }
    return rom_word_at(DM_SPIN_LOOP);
}

/* --- Classic: async .Sony completion (Shut Down / Eject fix) ---------------
 * The DM intercepts below complete .Sony requests on the spot and return RTS.
 * For an ASYNC call (trap word in D1 has bit 10 set) the caller expects its
 * ioCompletion routine to run when the request finishes; the File Manager in
 * particular issues async driver calls (e.g. _Eject -> Control csCode 7 with
 * completion $4043A0) and its FS queue only advances from that completion.
 * Never calling it left _Eject (Finder Special > Shut Down / Restart, and
 * Eject) waiting forever on ioResult=1 of the FS PB.
 * Fix: chain the completion like a driver that finishes immediately (IODone
 * runs the completion before the trap returns). The returned RTS pops the
 * completion address, the completion runs with A0=PB, D0=ioResult=0 (Z set),
 * and its RTS lands on ROM $403302 (MOVEQ #0,D0; RTS), which returns to the
 * trap dispatcher with D0=noErr. Call with A0's PB (a0m) before clearing
 * ioCompletion; only acts when the PC is at the intercepted entry. */
#if !MAC_ROM_LC_II
#define DM_RET_NOERR 0x403302   /* Classic ROM: MOVEQ #0,D0; RTS */
static void dm_async_complete(uint32_t a0m, uint32_t entry)
{
    uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
    if (pc != entry && pc != entry + 2) return;          /* not a fetch */
    if (!(m68k_get_reg(NULL, M68K_REG_D1) & 0x0400)) return;  /* sync */
    uint32_t compl = READ_LONG(ram, a0m + 0x0C) & 0xFFFFFF;
    if (!compl) return;
    uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP) - 8;
    uint32_t spm = sp & ram_mask;
    if (spm + 8 > ram_size) return;
    WRITE_LONG(ram, spm, compl);          /* RTS -> completion routine */
    WRITE_LONG(ram, spm + 4, DM_RET_NOERR); /* its RTS -> D0=0; RTS */
    m68k_set_reg(M68K_REG_SP, sp);
    m68k_set_reg(M68K_REG_SR, (m68k_get_reg(NULL, M68K_REG_SR) & ~0x0F) | 0x04);
    static int ac_cnt = 0;
    if (ac_cnt < 30) {
        ESP_LOGW(TAG, "DM-ASYNC #%d entry=$%06X PB=$%06X compl=$%06X",
                 ac_cnt, entry, a0m, compl);
        ac_cnt++;
    }
}
#endif
/* --- end async .Sony completion --- */

/* Intercept Device Manager _Read handler at $40315C.
 * When PB ioRefNum = -5 (.SONY), fulfill from disk image and return RTS. */
static uint16_t read16_dm_read(void)
{
    uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF;
    uint32_t a0m = a0 & ram_mask;
    if (ram && a0m + 0x32 < ram_size) {
        int16_t ioRefNum = (int16_t)((ram[a0m+0x18]<<8)|ram[a0m+0x19]);
        if (ioRefNum == -5) {
            static int dm_cnt = 0;
            uint32_t io_buf = READ_LONG(ram, a0m+0x20);
            uint32_t io_cnt = READ_LONG(ram, a0m+0x24);
            int32_t  io_off = (int32_t)READ_LONG(ram, a0m+0x2E);
            uint16_t ioTrap = READ_WORD(ram, a0m+0x06);
            uint32_t buf_m  = io_buf & ram_mask;
            uint8_t *disk = iwm_get_disk_data();
            uint32_t dsize = iwm_get_disk_size();

            if (disk && io_cnt > 0 && io_cnt <= 0x100000 &&
                buf_m + io_cnt <= ram_size &&
                io_off >= 0 && (uint32_t)io_off + io_cnt <= dsize) {
                disk_ov_read(ram + buf_m, io_off, io_cnt);   /* overlay, else flash */
                ram[a0m+0x28]=ram[a0m+0x24]; ram[a0m+0x29]=ram[a0m+0x25];
                ram[a0m+0x2A]=ram[a0m+0x26]; ram[a0m+0x2B]=ram[a0m+0x27];
                if (dm_cnt < 200) {
                    ESP_LOGW(TAG, "DM-READ #%d buf=$%06X off=%ld cnt=%lu trap=%04X",
                             dm_cnt, io_buf & 0xFFFFFF, (long)io_off,
                             (unsigned long)io_cnt, ioTrap);
                }
            } else if (dm_cnt < 200) {
                ESP_LOGW(TAG, "DM-READ #%d NOFILL buf=$%06X off=%ld cnt=%lu dsize=%lu",
                         dm_cnt, io_buf & 0xFFFFFF, (long)io_off,
                         (unsigned long)io_cnt, (unsigned long)dsize);
            }
#if !MAC_ROM_LC_II
            dm_async_complete(a0m, DM_READ_ENTRY);  /* async: run ioCompletion */
#endif
            ram[a0m+0x0C] = 0; ram[a0m+0x0D] = 0;
            ram[a0m+0x0E] = 0; ram[a0m+0x0F] = 0;
            ram[a0m+0x10] = 0; ram[a0m+0x11] = 0;
            m68k_set_reg(M68K_REG_D0, 0);
            dm_cnt++;
            return 0x4E75;  /* RTS */
        }
    }
    return rom_word_at(DM_READ_ENTRY);
}

/* --- Classic writable disk: DM _Write intercept ($4031CC) ---------------
 * Like read16_dm_read: a .Sony (ioRefNum -5) _Write is completed on the spot
 * into the PSRAM overlay (mac_disk_overlay.c), sync or async (ioCompletion
 * chained by dm_async_complete).  Pool full -> ioResult = dskFulErr, nothing
 * written.  Only active when the overlay is (iwm_disk_writable); otherwise
 * the ROM word is returned and the old read-only path runs. */
#if !MAC_ROM_LC_II && DISK_WRITABLE
static uint16_t read16_dm_write(void)
{
    uint32_t a0m = m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF & ram_mask;
    if (!iwm_disk_writable || !ram || a0m + 0x32 >= ram_size ||
        (int16_t)READ_WORD(ram, a0m+0x18) != -5)
        return rom_word_at(DM_WRITE_ENTRY);
    static int dw_cnt = 0;
    uint32_t io_buf = READ_LONG(ram, a0m+0x20);
    uint32_t io_cnt = READ_LONG(ram, a0m+0x24);
    int32_t  io_off = (int32_t)READ_LONG(ram, a0m+0x2E);
    uint32_t buf_m  = io_buf & ram_mask;
    uint32_t dsize  = iwm_get_disk_size();
    int16_t  err    = 0;
    uint32_t done   = 0;
    /* no disk-inserted check: the DM _Read intercept serves the volume
     * before the IWM's deferred insert, and the mount writes the MDB then */
    if (io_cnt > 0x100000 || buf_m + io_cnt > ram_size ||
               io_off < 0 || (uint32_t)io_off + io_cnt > dsize) {
        err = -50;                                  /* paramErr */
    } else if (io_cnt) {
        err = (int16_t)disk_ov_write(ram + buf_m, (uint32_t)io_off, io_cnt);
        if (!err) done = io_cnt;
    }
    WRITE_LONG(ram, a0m+0x28, done);                /* ioActCount */
    if (dw_cnt < 40 || err) {
        ESP_LOGW(TAG, "DM-WRITE #%d buf=$%06X off=%ld cnt=%lu %s err=%d",
                 dw_cnt, io_buf & 0xFFFFFF, (long)io_off, (unsigned long)io_cnt,
                 (m68k_get_reg(NULL, M68K_REG_D1) & 0x0400) ? "async" : "sync", err);
    }
    dw_cnt++;
    ram[a0m+0x10] = (uint8_t)((uint16_t)err >> 8);  /* ioResult */
    ram[a0m+0x11] = (uint8_t)err;
    m68k_set_reg(M68K_REG_D0, (uint32_t)(int32_t)err);
    dm_async_complete(a0m, DM_WRITE_ENTRY);         /* async: run ioCompletion */
    if (err)   /* completion / caller test the result: N set, Z clear */
        m68k_set_reg(M68K_REG_SR, (m68k_get_reg(NULL, M68K_REG_SR) & ~0x0F) | 0x08);
    ram[a0m+0x0C] = 0; ram[a0m+0x0D] = 0;           /* ioCompletion */
    ram[a0m+0x0E] = 0; ram[a0m+0x0F] = 0;
    return 0x4E75;  /* RTS */
}
#endif
/* --- end writable disk DM _Write --- */

/* Intercept Device Manager _Control handler at $4031E0.
 * When PB ioRefNum = -5 (.SONY), return noErr immediately. */
static uint16_t read16_dm_ctl(void)
{
    uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF;
    uint32_t a0m = a0 & ram_mask;
    if (ram && a0m + 0x1C < ram_size) {
        int16_t ioRefNum = (int16_t)((ram[a0m+0x18]<<8)|ram[a0m+0x19]);
        if (ioRefNum == -5) {
            static int ctl_cnt = 0;
            int16_t csCode = (int16_t)((ram[a0m+0x1A]<<8)|ram[a0m+0x1B]);
            if (ctl_cnt < 30) {
                ESP_LOGW(TAG, "DM-CTL #%d csCode=%d ref=%d PC=%06X",
                         ctl_cnt, csCode, ioRefNum,
                         m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
            }
#if !MAC_ROM_LC_II
            dm_async_complete(a0m, DM_CTL_ENTRY);  /* async: run ioCompletion */
#endif
            ram[a0m+0x0C] = 0; ram[a0m+0x0D] = 0;
            ram[a0m+0x0E] = 0; ram[a0m+0x0F] = 0;
            ram[a0m+0x10] = 0; ram[a0m+0x11] = 0;
            m68k_set_reg(M68K_REG_D0, 0);
            ctl_cnt++;
            return 0x4E75;  /* RTS */
        }
    }
    return rom_word_at(DM_CTL_ENTRY);
}

/* Intercept Device Manager _Status handler at $4031EE.
 * When PB ioRefNum = -5 (.SONY), return noErr.
 * For csCode=8 (driveStatus), fill csParam from DrvSts at $0040. */
static uint16_t read16_dm_stat(void)
{
    uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF;
    uint32_t a0m = a0 & ram_mask;
    if (ram && a0m + 0x1C < ram_size) {
        int16_t ioRefNum = (int16_t)((ram[a0m+0x18]<<8)|ram[a0m+0x19]);
        if (ioRefNum == -5) {
            static int stat_cnt = 0;
            int16_t csCode = (int16_t)((ram[a0m+0x1A]<<8)|ram[a0m+0x1B]);
            if (stat_cnt < 30) {
                ESP_LOGW(TAG, "DM-STAT #%d csCode=%d ref=%d PC=%06X",
                         stat_cnt, csCode, ioRefNum,
                         m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
            }
            if (csCode == 8 && a0m + 0x3C < ram_size) {
                /* DrvSts at $0040 (moved from heap to reserved vectors) */
                ram[a0m+0x1C] = ram[0x0040]; /* track high */
                ram[a0m+0x1D] = ram[0x0041]; /* track low */
                ram[a0m+0x1E] = ram[0x0042]; /* writeProt */
                ram[a0m+0x1F] = ram[0x0043]; /* diskInPlace */
                ram[a0m+0x20] = ram[0x0044]; /* installed */
                ram[a0m+0x21] = ram[0x0045]; /* sides */
            }
            if (csCode == 6 && a0m + 0x22 < ram_size) {
                /* Return Format List: one entry describing the inserted
                 * image (any size, e.g. a 2880K HFS volume). Without it the
                 * caller (File Manager, after mounting a non-floppy-sized
                 * volume) used stale csParam and read past the image. */
                uint32_t lst = READ_LONG(ram, a0m+0x1E) & ram_mask;
                uint16_t maxn = READ_WORD(ram, a0m+0x1C);
                uint32_t blocks = iwm_get_disk_size() / 512;
                if (maxn >= 1 && blocks && lst + 8 <= ram_size) {
                    WRITE_LONG(ram, lst, blocks);   /* volume size, blocks */
                    ram[lst+4] = 0x40;              /* current disk format */
                    ram[lst+5] = 0;                 /* sectors/track: n/a */
                    ram[lst+6] = 0; ram[lst+7] = 0; /* tracks: n/a */
                    ram[a0m+0x1C] = 0; ram[a0m+0x1D] = 1;
                } else {
                    ram[a0m+0x1C] = 0; ram[a0m+0x1D] = 0;
                }
            }
#if !MAC_ROM_LC_II
            dm_async_complete(a0m, DM_STAT_ENTRY);  /* async: run ioCompletion */
#endif
            ram[a0m+0x0C] = 0; ram[a0m+0x0D] = 0;
            ram[a0m+0x0E] = 0; ram[a0m+0x0F] = 0;
            ram[a0m+0x10] = 0; ram[a0m+0x11] = 0;
            m68k_set_reg(M68K_REG_D0, 0);
            stat_cnt++;
            return 0x4E75;  /* RTS */
        }
    }
    return rom_word_at(DM_STAT_ENTRY);
}

/* Patch PB at sub6 entry ($400DF4): set ioRefNum = -5 (.SONY).
 * The NOP bypass at $400CAC skips event processing that normally
 * populates ioRefNum. Does NOT replace the instruction — falls through. */
static void read16_sub6_patch(void)
{
    static int read_log = 0;
    uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF;
    uint32_t masked = a0 & ram_mask;
    if (ram && masked + 0x19 < ram_size) {
        ram[masked + 0x18] = 0xFF;  /* ioRefNum high byte */
        ram[masked + 0x19] = 0xFB;  /* ioRefNum low byte = -5 */
    }
    if (read_log < 3) {
        ESP_LOGW(TAG, "READ-SUB6 A0=%06X ioRef=FFFB(patched) A6=%06X D4=%04X",
                 a0,
                 m68k_get_reg(NULL, M68K_REG_A6) & 0xFFFFFF,
                 (uint16_t)m68k_get_reg(NULL, M68K_REG_D4));
        read_log++;
    }
}

/* Intercept _Read trap at $400E0A (A002 opcode).
 * Replace with MOVEQ #0,D0 ($7000) which sets D0=0 AND Z=1.
 * Z=1 makes BNE at $400E0C fall through to boot block sig check. */
static uint16_t read16_read_trap(void)
{
    static int trap_count = 0;
    uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF;
    uint32_t a0m = a0 & ram_mask;
    uint32_t io_buf = 0, io_cnt = 0;
    int32_t io_off = 0;
    if (ram && a0m + 0x31 < ram_size) {
        io_buf = ((uint32_t)ram[a0m+0x20]<<24)|((uint32_t)ram[a0m+0x21]<<16)|
                 ((uint32_t)ram[a0m+0x22]<<8)|ram[a0m+0x23];
        io_cnt = ((uint32_t)ram[a0m+0x24]<<24)|((uint32_t)ram[a0m+0x25]<<16)|
                 ((uint32_t)ram[a0m+0x26]<<8)|ram[a0m+0x27];
        io_off = (int32_t)(((uint32_t)ram[a0m+0x2E]<<24)|((uint32_t)ram[a0m+0x2F]<<16)|
                           ((uint32_t)ram[a0m+0x30]<<8)|ram[a0m+0x31]);
    }
    uint32_t buf_m = io_buf & ram_mask;
    uint8_t *disk = iwm_get_disk_data();
    uint32_t dsize = iwm_get_disk_size();

    if (disk && ram && io_cnt > 0 && io_cnt <= 0x10000 &&
        buf_m + io_cnt <= ram_size && (uint32_t)io_off + io_cnt <= dsize) {
        disk_ov_read(ram + buf_m, io_off, io_cnt);
        ram[a0m+0x28] = ram[a0m+0x24]; ram[a0m+0x29] = ram[a0m+0x25];
        ram[a0m+0x2A] = ram[a0m+0x26]; ram[a0m+0x2B] = ram[a0m+0x27];
        ram[a0m+0x0C] = 0; ram[a0m+0x0D] = 0;
        ram[a0m+0x0E] = 0; ram[a0m+0x0F] = 0;
        ram[a0m+0x10] = 0; ram[a0m+0x11] = 0;
        m68k_set_reg(M68K_REG_D0, 0);
        if (trap_count < 5) {
            ESP_LOGW(TAG, "DISK-READ: buf=%06X off=%ld cnt=%lu sig=%02X%02X",
                     io_buf, (long)io_off, (unsigned long)io_cnt,
                     ram[buf_m], ram[buf_m+1]);
        }
        trap_count++;
        return 0x7000;  /* MOVEQ #0,D0: D0=0, Z=1 */
    } else {
        if (trap_count < 5) {
            ESP_LOGW(TAG, "DISK-READ: SKIP buf=%06X off=%ld cnt=%lu dsize=%lu",
                     io_buf, (long)io_off, (unsigned long)io_cnt,
                     (unsigned long)dsize);
        }
        trap_count++;
    }
    return rom_word_at(BOOT_READ_HELPER);
}

/* .SONY driver Prime intercept ($434B76): Device Manager calls
 * driver's Prime routine via DCE, bypassing OS trap table.
 * Fulfill read from disk image and return RTS. */
static uint16_t read16_prime(void)
{
    uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF;
    uint32_t a0m = a0 & ram_mask;
    if (ram && a0m + 0x32 < ram_size) {
        static int prime_cnt = 0;
        uint32_t io_buf = READ_LONG(ram, a0m+0x20);
        uint32_t io_cnt = READ_LONG(ram, a0m+0x24);
        int32_t  io_off = (int32_t)READ_LONG(ram, a0m+0x2E);
        uint16_t ioTrap = READ_WORD(ram, a0m+0x06);
        uint32_t buf_m  = io_buf & ram_mask;
        uint8_t *disk = iwm_get_disk_data();
        uint32_t dsize = iwm_get_disk_size();

        if (disk && io_cnt > 0 && io_cnt <= 0x100000 &&
            buf_m + io_cnt <= ram_size &&
            io_off >= 0 && (uint32_t)io_off + io_cnt <= dsize) {
            disk_ov_xfer((ioTrap & 0xFF) == 0x03, ram + buf_m, io_off, io_cnt);
            ram[a0m+0x28]=ram[a0m+0x24]; ram[a0m+0x29]=ram[a0m+0x25];
            ram[a0m+0x2A]=ram[a0m+0x26]; ram[a0m+0x2B]=ram[a0m+0x27];
            if (prime_cnt < 50) {
                ESP_LOGW(TAG, "PRIME-RD #%d buf=$%06X off=%ld cnt=%lu trap=%04X",
                         prime_cnt, io_buf & 0xFFFFFF, (long)io_off,
                         (unsigned long)io_cnt, ioTrap);
            }
        } else if (prime_cnt < 50) {
            ESP_LOGW(TAG, "PRIME #%d NOFILL buf=$%06X off=%ld cnt=%lu",
                     prime_cnt, io_buf & 0xFFFFFF,
                     (long)io_off, (unsigned long)io_cnt);
        }
        ram[a0m+0x0C] = 0; ram[a0m+0x0D] = 0;
        ram[a0m+0x0E] = 0; ram[a0m+0x0F] = 0;
        ram[a0m+0x10] = 0; ram[a0m+0x11] = 0;
        m68k_set_reg(M68K_REG_D0, 0);
        prime_cnt++;
        return 0x4E75;  /* RTS */
    }
    return rom_word_at(0x434B76);
}

/* .SONY driver entry intercept ($434BAC): ROM's File Manager calls
 * driver via JSR. Fulfill reads from disk image, succeed silently
 * for control/status calls. Return RTS to immediately return. */
static uint16_t read16_sony(void)
{
    static int sony_cnt = 0;
    uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF;
    uint32_t a0m = a0 & ram_mask;
    if (ram && a0m + 0x32 < ram_size) {
        uint16_t ioTrap = (ram[a0m+0x06]<<8)|ram[a0m+0x07];
        int16_t ioRefNum = (int16_t)((ram[a0m+0x18]<<8)|ram[a0m+0x19]);
        uint16_t csCode = (ram[a0m+0x1A]<<8)|ram[a0m+0x1B];
        uint32_t io_buf = ((uint32_t)ram[a0m+0x20]<<24)|
            ((uint32_t)ram[a0m+0x21]<<16)|
            ((uint32_t)ram[a0m+0x22]<<8)|ram[a0m+0x23];
        uint32_t io_cnt = ((uint32_t)ram[a0m+0x24]<<24)|
            ((uint32_t)ram[a0m+0x25]<<16)|
            ((uint32_t)ram[a0m+0x26]<<8)|ram[a0m+0x27];
        int32_t io_off = (int32_t)(((uint32_t)ram[a0m+0x2E]<<24)|
            ((uint32_t)ram[a0m+0x2F]<<16)|
            ((uint32_t)ram[a0m+0x30]<<8)|ram[a0m+0x31]);
        uint8_t *disk = iwm_get_disk_data();
        uint32_t dsize = iwm_get_disk_size();
        uint32_t buf_m = io_buf & ram_mask;

        if (sony_cnt < 100 || (sony_cnt % 200 == 0)) {
            ESP_LOGW(TAG, "SONY #%d trap=%04X ref=%d cs=%d buf=$%06X off=%ld cnt=%lu",
                     sony_cnt, ioTrap, ioRefNum, csCode,
                     io_buf & 0xFFFFFF, (long)io_off, (unsigned long)io_cnt);
        }

        if (disk && io_cnt > 0 && io_cnt <= 0x100000 &&
            buf_m + io_cnt <= ram_size &&
            io_off >= 0 && (uint32_t)io_off + io_cnt <= dsize) {
            disk_ov_xfer((ioTrap & 0xFF) == 0x03, ram + buf_m, io_off, io_cnt);
            ram[a0m+0x28]=ram[a0m+0x24]; ram[a0m+0x29]=ram[a0m+0x25];
            ram[a0m+0x2A]=ram[a0m+0x26]; ram[a0m+0x2B]=ram[a0m+0x27];
            if (sony_cnt < 100) {
                ESP_LOGW(TAG, "SONY-RD #%d buf=$%06X off=%ld cnt=%lu",
                         sony_cnt, io_buf & 0xFFFFFF,
                         (long)io_off, (unsigned long)io_cnt);
            }
        } else if (io_cnt > 0) {
            if (sony_cnt < 100) {
                ESP_LOGW(TAG, "SONY #%d NOFILL buf=$%06X off=%ld cnt=%lu",
                         sony_cnt, io_buf & 0xFFFFFF,
                         (long)io_off, (unsigned long)io_cnt);
            }
        } else {
            if (sony_cnt < 50) {
                ESP_LOGW(TAG, "SONY-CTL #%d cs=%d", sony_cnt, csCode);
            }
        }
        ram[a0m+0x0C] = 0; ram[a0m+0x0D] = 0;
        ram[a0m+0x0E] = 0; ram[a0m+0x0F] = 0;
        ram[a0m+0x10] = 0; ram[a0m+0x11] = 0;
        m68k_set_reg(M68K_REG_D0, 0);
        sony_cnt++;
        return 0x4E75;  /* RTS */
    }
    return rom_word_at(0x434BAC);
}

MEM_CB_ATTR unsigned int m68k_read_memory_16(unsigned int address)
{
    address &= 0x00FFFFFF;
#if MAC_ROM_LC_II
#if MAC_DIAG
    uint32_t v8_prev_fetch = v8_fetch_ring[(v8_fetch_idx - 1) & 255];
    v8_fetch_ring[v8_fetch_idx & 255] = address;
    v8_fetch_idx++;
    static int v8_zrun = 0;
    static bool v8_zw_done = false;
    if (!v8_zw_done && address >= 0x400000 && address < 0x5FFFD2) {
        uint32_t ra = address & ram_mask;
        if ((ram[ra] | ram[ra + 1]) != 0) {
            v8_zrun = 0;
        } else if (address == v8_prev_fetch + 2) {
            if (++v8_zrun == 6) {
                v8_zw_done = true;
                ESP_LOGW(TAG, "V8ZW: zero-walk at $%06X prev=$%06X PC=$%06X ring:",
                         address, v8_prev_fetch,
                         m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
                for (int ri = 24; ri >= 9; ri -= 8) {
                    int base = (v8_fetch_idx - ri) & 255;
                    ESP_LOGW(TAG, "  %06X %06X %06X %06X %06X %06X %06X %06X",
                             v8_fetch_ring[base], v8_fetch_ring[(base+1)&255],
                             v8_fetch_ring[(base+2)&255], v8_fetch_ring[(base+3)&255],
                             v8_fetch_ring[(base+4)&255], v8_fetch_ring[(base+5)&255],
                             v8_fetch_ring[(base+6)&255], v8_fetch_ring[(base+7)&255]);
                }
            }
        }
    }
    static int v8sm = 0;
    if (0 && v8sm < 400 && address >= 0x5FFFD2 && address < 0x610000) {
        if (v8sm == 0) {
            uint32_t sp0 = m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF;
            ESP_LOGW(TAG, "V8SM0 SP=$%06X stack SP-32..SP+32:", sp0);
            for (int si = 0; si < 64; si += 16) {
                uint32_t sa = (sp0 - 32 + si) & ram_mask;
                ESP_LOGW(TAG, "  $%06X %08X %08X %08X %08X", sa,
                         (ram[sa] << 24) | (ram[sa + 1] << 16) | (ram[sa + 2] << 8) | ram[sa + 3],
                         (ram[sa + 4] << 24) | (ram[sa + 5] << 16) | (ram[sa + 6] << 8) | ram[sa + 7],
                         (ram[sa + 8] << 24) | (ram[sa + 9] << 16) | (ram[sa + 10] << 8) | ram[sa + 11],
                         (ram[sa + 12] << 24) | (ram[sa + 13] << 16) | (ram[sa + 14] << 8) | ram[sa + 15]);
            }
            ESP_LOGW(TAG, "V8SM-JUMP ring before walk:");
            for (int ri = 24; ri >= 1; ri -= 8) {
                int base = ((v8_fetch_idx - ri) & 255);
                ESP_LOGW(TAG, "  %06X %06X %06X %06X %06X %06X %06X %06X",
                         v8_fetch_ring[base], v8_fetch_ring[(base+1)&255],
                         v8_fetch_ring[(base+2)&255], v8_fetch_ring[(base+3)&255],
                         v8_fetch_ring[(base+4)&255], v8_fetch_ring[(base+5)&255],
                         v8_fetch_ring[(base+6)&255], v8_fetch_ring[(base+7)&255]);
            }
        }
        uint32_t ra = address & ram_mask;
        ESP_LOGW(TAG, "V8SM #%d: $%06X w=%04X PC=$%06X", v8sm++, address,
                 (address >= 0x5FFFD2 && address < 0x610000)
                     ? (rom[0x0FFD2 + (address - 0x5FFFD2)] << 8) | rom[0x0FFD3 + (address - 0x5FFFD2)]
                     : (ram[ra] << 8) | ram[ra + 1],
                 m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
    }
    static bool v8_exit_done = false;
    if (!v8_exit_done && v8sm > 0 &&
        !((address >= 0x5FFFD2 && address < 0x610000)) &&
        ((v8_prev_fetch >= 0x5FFFD2 && v8_prev_fetch < 0x610000))) {
        v8_exit_done = true;
        ESP_LOGW(TAG, "V8EXIT ring (flow left alias): SP=$%06X PC=$%06X",
                 m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF,
                 m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        uint32_t spx = m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF;
        ESP_LOGW(TAG, "V8EXIT stack SP-32..SP+32 (SP=$%06X):", spx);
        for (int si = 0; si < 64; si += 16) {
            uint32_t sa = (spx - 32 + si) & ram_mask;
            ESP_LOGW(TAG, "  $%06X %08X %08X %08X %08X", sa,
                     (ram[sa] << 24) | (ram[sa + 1] << 16) | (ram[sa + 2] << 8) | ram[sa + 3],
                     (ram[sa + 4] << 24) | (ram[sa + 5] << 16) | (ram[sa + 6] << 8) | ram[sa + 7],
                     (ram[sa + 8] << 24) | (ram[sa + 9] << 16) | (ram[sa + 10] << 8) | ram[sa + 11],
                     (ram[sa + 12] << 24) | (ram[sa + 13] << 16) | (ram[sa + 14] << 8) | ram[sa + 15]);
        }
        for (int ri = 24; ri >= 1; ri -= 8) {
            int base = ((v8_fetch_idx - ri) & 255);
            ESP_LOGW(TAG, "  %06X %06X %06X %06X %06X %06X %06X %06X",
                     v8_fetch_ring[base], v8_fetch_ring[(base+1)&255],
                     v8_fetch_ring[(base+2)&255], v8_fetch_ring[(base+3)&255],
                     v8_fetch_ring[(base+4)&255], v8_fetch_ring[(base+5)&255],
                     v8_fetch_ring[(base+6)&255], v8_fetch_ring[(base+7)&255]);
        }
    }
    static int v8r100 = 0;
    if (v8r100 < 40 && address >= 0x100000 && address < 0x100400) {
        uint32_t ra = address & ram_mask;
        ESP_LOGW(TAG, "V8R100 #%d: $%06X w=%04X PC=$%06X", v8r100++, address,
                 (ram[ra] << 8) | ram[ra + 1],
                 m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
    }
#endif /* MAC_DIAG */
#endif

    /* ROM patches — Mini vMac approach (ROMEMDEV.c).
     *
     * Mac Classic ROM boot flow:
     *   $002A → $0040 (MOVE SR) → $1BDE (main init) → ... → $1D16 (JMP $0048)
     *   $0048: BSR calls (SWIM/$01D4, sizing, SCC, etc.)
     *   $0064: JSR $1D3E (first RAM test pass — needed for sizing)
     *   $0086: JSR $1D3E (second pass — destructive, skip)
     *   $011E: JSR $1D3E (third pass — destructive, skip)
     *
     * Mini vMac patches:
     *   $1C68 → $6008: BRA.S +8 (skip ROM checksum)
     *   $0086 → $6002: BRA.S +2 (skip 2nd RAM test = JSR $1D3E)
     *   $011E → $6002: BRA.S +2 (skip 3rd RAM test = JSR $1D3E)
     *
     * The first RAM test pass (from $1CC4/$1CDA/$1CEC → $26F0/$25F8/$2928)
     * runs normally and handles sizing. Mini vMac lets it run.
     * (Applied just below the RAM fast path.) */

    /* Fast path: normal mode RAM read — by far the most common case.
     * Skips all intercept checks for addresses below $400000.
     * Alias via ram_mask (power-of-2) so the ROM's sizing code
     * sees correct hardware-style aliasing at the RAM boundary. */
#if MAC_ROM_LC_II
    if (__builtin_expect(!via_overlay_fast &&
        (address < ram_size), 1)) {
#else
    if (__builtin_expect(!via_overlay_fast && address < 0x400000, 1)) {
#endif
#if !MAC_ROM_LC_II
        /* Fix system-patched _InitWindows at $B322: the System file's ptch
         * resource patches TB[$112] to $B322, but that address gets overwritten
         * by the resource map type list (heap corruption).
         * Instead of writing a JMP trampoline (which failed — the 32-bit
         * target fetch may read stale RAM), redirect execution by setting
         * the PC directly to ROM's _InitWindows and returning its opcode. */
        if (__builtin_expect(mac_boot_read_bypass && address == 0x00B322, 0)) {
            static int iw_sys = 0;
            uint32_t cur_pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            uint16_t first_w = (ram[0xB322] << 8) | ram[0xB323];
            /* Log ALL accesses to $B322 — data or instruction fetch */
            if (iw_sys < 10) {
                ESP_LOGW(TAG, "$B322-ACCESS #%d: PC=$%06X data=$%04X SP=$%06X",
                         iw_sys, cur_pc, first_w,
                         m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
            }
            /* Instruction fetch: m68ki_read_imm_16 does REG_PC += 2 then
             * reads (REG_PC-2), so PC == $B324 here for opcode fetch. */
            if (cur_pc == 0x00B324) {
                if (first_w != 0x4E56 && first_w != 0x48E7) {
                    /* $B322 lacks a valid function prologue — overwritten
                     * by resource map type list.  Redirect to ROM. */
                    ESP_LOGW(TAG, "FIX: _InitWindows $B322=$%04X -> ROM $40BBDA",
                             first_w);
                    /* Fix trap table TB[$112] for future calls */
                    ram[0x1248] = 0x00; ram[0x1249] = 0x40;
                    ram[0x124A] = 0xBB; ram[0x124B] = 0xDA;
                    /* Redirect: set PC past ROM's first opcode, return it */
                    m68k_set_reg(M68K_REG_PC, 0x40BBDC);
                    uint16_t rom_op = (rom[0xBBDA] << 8) | rom[0xBBDB];
                    ESP_LOGW(TAG, "  ROM opcode=$%04X new PC=$40BBDC", rom_op);
                    iw_sys++;
                    return rom_op;
                }
                /* $B322 has valid code — let it execute normally */
                ESP_LOGW(TAG, "*** _InitWindows(SYS) #%d $B322=$%04X SP=$%06X",
                         iw_sys, first_w,
                         m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
            }
            iw_sys++;
        }
#endif
        uint32_t ra = address & ram_mask;
        ARM_FAST_RD();
        if (__builtin_expect(ra + 1 < ram_size, 1)) {
            return (ram[ra] << 8) | ram[ra + 1];
        }
        /* Rare: access spans aliasing boundary */
        return (ram[ra] << 8) | ram[(address + 1) & ram_mask];
    }

    /* (Moved below the RAM fast path: it only matches ROM offsets, which
     * that path never serves, so RAM data reads skip these compares.) */
#if !MAC_ROM_LC_II
    {
        uint32_t rom_off = 0xFFFFFFFF;
        if (address < 0x500000) {
            if (via_overlay_fast || address >= 0x400000) {
                rom_off = address & rom_mask;
            }
        }
        /* ROM checksum skip: BRA.S +8 at $1C68 (Mini vMac) */
        if (__builtin_expect(rom_off == 0x1C68, 0)) {
            static int rc = 0;
            if (rc++ < 3) ESP_LOGW(TAG, "ROM-PATCH: skip checksum @$%06X", address);
            return 0x6008;  /* BRA.S +8 */
        }
        /* Skip 2nd destructive RAM test: BRA.S +2 at $0086 (Mini vMac) */
        if (__builtin_expect(rom_off == 0x0086, 0)) {
            static int rt2 = 0;
            if (rt2++ < 3) ESP_LOGW(TAG, "ROM-PATCH: skip RAM test2 @$%06X", address);
            return 0x6002;  /* BRA.S +2 */
        }
        /* Skip 3rd destructive RAM test: BRA.S +2 at $011E (Mini vMac) */
        if (__builtin_expect(rom_off == 0x011E, 0)) {
            static int rt3 = 0;
            if (rt3++ < 3) ESP_LOGW(TAG, "ROM-PATCH: skip RAM test3 @$%06X", address);
            return 0x6002;  /* BRA.S +2 */
        }
    }
#endif

    /* Normal mode ROM read ($400000-$4FFFFF).
     * Check intercepts only for specific addresses via if-chain.
     * For the vast majority of ROM reads (instruction fetches), no
     * intercept matches and we return ROM data directly.
     * The old approach checked all intercepts on every 16-bit read;
     * this limits intercept checks to ROM-range reads only. */
#if MAC_ROM_LC_II
    if (__builtin_expect(!via_overlay_fast && address >= 0xA00000 && address < 0xB00000, 1)) {
#else
    if (__builtin_expect(!via_overlay_fast && address >= 0x400000 && address < 0x500000, 1)) {
#endif
        if (__builtin_expect(mac_boot_read_bypass, 0)) {
#if !MAC_ROM_LC_II
            if (address == 0x400CAC) return 0x4E71; /* BEQ→NOP */
#endif

            /* CRITICAL FIX: Patch UNIMP trap handler to return safely.
             * The ROM's handler at $400768 does MOVEQ #12,D0; BRA $40138A
             * which jumps to the SYSTEM ERROR handler (crash dialog).
             * When ptch resource code calls a not-yet-patched trap, this
             * kills the boot process.  Replace with:
             *   SUBA.L A0,A0  ; A0 = NULL (safe handle)
             *   MOVEQ  #0,D0  ; no error
             *   RTS            ; return to caller */
#if !MAC_ROM_LC_II
            if (address == 0x400768) return 0x91C8;  /* SUBA.L A0,A0 */
            if (address == 0x40076A) return 0x7000;  /* MOVEQ #0, D0 */
            if (address == 0x40076C) return 0x4E75;  /* RTS */
#endif

#if !MAC_ROM_LC_II
            if (address == DM_READ_ENTRY) return read16_dm_read();
            if (address == DM_SPIN_LOOP) return read16_dm_spin_rescue();
            if (address == DM_CTL_ENTRY) return read16_dm_ctl();
            if (address == DM_STAT_ENTRY) return read16_dm_stat();
#if DISK_WRITABLE
            if (address == DM_WRITE_ENTRY) return read16_dm_write();  /* writable disk */
#endif
            if (address == 0x400DF4) read16_sub6_patch();  /* patches PB, falls through */
            if (address == BOOT_READ_HELPER) return read16_read_trap();
#endif
#if !MAC_ROM_LC_II
            if (address == 0x434B76) return read16_prime();
            if (address == 0x434BAC) return read16_sony();
#endif

#if !MAC_ROM_LC_II
#if MAC_DIAG
            /* Detect _InitGraf entry ($412EDC = TB[$06E] ROM handler). */
            if (address == 0x412EDC) {
                static int ig_cnt = 0;
                if (ig_cnt < 3) {
                    ESP_LOGW(TAG, "*** _InitGraf #%d *** PC=$%06X SP=$%06X",
                             ig_cnt,
                             m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF,
                             m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
                }
                ig_cnt++;
                initgraf_seen = 1;
            }

            /* Detect _InitFonts ROM entry ($419150 = TB[$0FE]). */
            if (address == 0x419150) {
                static int if_cnt = 0;
                if (if_cnt < 3) {
                    ESP_LOGW(TAG, "*** _InitFonts #%d *** PC=$%06X SP=$%06X",
                             if_cnt,
                             m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF,
                             m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
                }
                if_cnt++;
            }
            /* Detect _InitWindows ROM entry ($40BBDA = TB[$112]). */
            if (address == 0x40BBDA) {
                static int iw_rom_cnt = 0;
                if (iw_rom_cnt < 10) {
                    uint32_t wmgr = ((uint32_t)ram[0x9DE]<<24)|((uint32_t)ram[0x9DF]<<16)|
                                    ((uint32_t)ram[0x9E0]<<8)|ram[0x9E1];
                    ESP_LOGW(TAG, "*** _InitWindows(ROM) #%d *** SP=$%06X WMgr=$%08lX",
                             iw_rom_cnt,
                             m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF,
                             (unsigned long)wmgr);
                    iw_rom_cnt++;
                }
            }
            /* Detect _InitMenus ROM entry ($40C97C = TB[$130]). */
            if (address == 0x40C97C) {
                static int im_cnt = 0;
                if (im_cnt < 3) {
                    ESP_LOGW(TAG, "*** _InitMenus #%d *** PC=$%06X SP=$%06X",
                             im_cnt,
                             m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF,
                             m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
                }
                im_cnt++;
            }
            /* Detect _GetNextEvent ROM entry ($40B5CC = TB[$0EA]). */
            if (address == 0x40B5CC) {
                static int gne_cnt = 0;
                if (gne_cnt < 5) {
                    ESP_LOGW(TAG, "*** _GetNextEvent #%d *** PC=$%06X SP=$%06X",
                             gne_cnt,
                             m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF,
                             m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
                }
                gne_cnt++;
            }
#endif /* MAC_DIAG */
            /* (Line-A trap logger removed — exception frame reading was unreliable) */

#if MAC_DIAG
            /* Detect _Launch entry ($4099A2 = TB[$1F2] ROM handler).
             * If this is ever fetched as an instruction, _Launch was called. */
            if (address == 0x4099A2) {
                static int launch_cnt = 0;
                if (launch_cnt < 10) {
                    uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0) & 0xFFFFFF;
                    uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF;
                    uint32_t d0 = m68k_get_reg(NULL, M68K_REG_D0);
                    /* On Mac, _Launch(A9F2) receives pfName pointer on stack.
                     * Log A0, SP, D0, and stack contents for diagnostics. */
                    uint8_t *rr = mac_mem_get_ram();
                    uint32_t rsz = mac_mem_get_ram_size();
                    char name[32] = {0};
                    /* Check CurApName at $0910 */
                    if (rr) {
                        int nl = rr[0x910];
                        if (nl > 0 && nl < 32)
                            for (int i = 0; i < nl; i++) name[i] = rr[0x911+i];
                    }
                    /* Read BootDrive */
                    uint16_t bd = rr ? (rr[0x210]<<8)|rr[0x211] : 0;
                    ESP_LOGW(TAG, "*** _LAUNCH #%d *** A0=$%06X SP=$%06X D0=$%08lX BD=%d CurApp=\"%s\"",
                             launch_cnt, a0, sp, (unsigned long)d0, bd, name);
                    /* Dump top of stack */
                    if (rr && sp + 15 < rsz) {
                        ESP_LOGW(TAG, "  Stack: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                                 rr[sp],rr[sp+1],rr[sp+2],rr[sp+3],
                                 rr[sp+4],rr[sp+5],rr[sp+6],rr[sp+7],
                                 rr[sp+8],rr[sp+9],rr[sp+10],rr[sp+11],
                                 rr[sp+12],rr[sp+13],rr[sp+14],rr[sp+15]);
                    }
                    launch_cnt++;
                }
            }
#endif /* MAC_DIAG */
#endif
        }

#if MAC_DIAG
        /* Detect UNIMPLEMENTED trap handler entry ($400768).
         * When an A-trap dispatches to $400768, this means the trap is
         * not installed.  Log the trap word (at return_addr-2 on stack)
         * to identify WHICH trap is being called. */
        if (__builtin_expect(address == 0x400768, 0)) {
            static int unimp_cnt = 0;
            if (unimp_cnt < 50) {
                uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF;
                uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
                uint8_t *rr = mac_mem_get_ram();
                uint32_t rsz = mac_mem_get_ram_size();
                /* On A-trap dispatch, stack has: [return addr pushed by JSR from Line-A handler].
                 * The return address points past the original trap word.
                 * Read a few stack words to identify the caller. */
                uint16_t trap_word = 0;
                uint32_t ret_addr = 0;
                if (rr && sp + 7 < rsz) {
                    /* Stack top is return address from Line-A dispatch */
                    ret_addr = ((uint32_t)rr[sp]<<24)|((uint32_t)rr[sp+1]<<16)|
                               ((uint32_t)rr[sp+2]<<8)|rr[sp+3];
                    /* Trap word is at ret_addr - 2 (before the return point) */
                    uint32_t tw_addr = (ret_addr - 2) & 0xFFFFFF;
                    if (tw_addr < rsz)
                        trap_word = (rr[tw_addr] << 8) | rr[tw_addr + 1];
                    else if (tw_addr >= 0x400000 && tw_addr < 0x500000) {
                        uint32_t ro = tw_addr & rom_mask;
                        trap_word = (rom[ro] << 8) | rom[ro + 1];
                    }
                }
                ESP_LOGW(TAG, "UNIMP-TRAP #%d: $%04X ret=$%06X PC=$%06X SP=$%06X",
                         unimp_cnt, trap_word, ret_addr, pc, sp);
                unimp_cnt++;
            }
        }

#endif /* MAC_DIAG */
        /* Non-intercepted ROM read — return data directly */
        uint32_t a = address & rom_mask;
        uint16_t w = (rom[a] << 8) | rom[a + 1];
        CLASSIC_NATIVE_PATCH(a, w);
        return w;
    }

#if MAC_ROM_LC_II
    if (VRAM_HIT(address, 2)) {
        const uint8_t *v = v8_vram + (address - V8_VRAM_BASE);
        return (v[0] << 8) | v[1];
    }
#endif
    /* Slow path: overlay mode, peripherals, exception handler logging */
    return (mem_read_byte(address) << 8) | mem_read_byte(address + 1);
}

MEM_CB_ATTR unsigned int m68k_read_memory_32(unsigned int address)
{
    address &= 0x00FFFFFF;
    /* Fast path: normal mode RAM read — alias via ram_mask */
#if MAC_ROM_LC_II
    if (__builtin_expect(!via_overlay_fast &&
        (address < ram_size), 1)) {
#else
    if (__builtin_expect(!via_overlay_fast && address < 0x400000, 1)) {
#endif
        uint32_t ra = address & ram_mask;
        ARM_FAST_RD();
        if (__builtin_expect(ra + 3 < ram_size, 1)) {
            return ((uint32_t)ram[ra] << 24) | ((uint32_t)ram[ra+1] << 16) |
                   ((uint32_t)ram[ra+2] << 8) | ram[ra+3];
        }
        /* Rare: access spans aliasing boundary */
        return ((uint32_t)ram[ra] << 24) |
               ((uint32_t)ram[(address+1) & ram_mask] << 16) |
               ((uint32_t)ram[(address+2) & ram_mask] << 8) |
               ram[(address+3) & ram_mask];
    }
    /* Fast path: normal mode ROM read */
#if MAC_ROM_LC_II
    if (__builtin_expect(!via_overlay_fast && address >= 0xA00000 && address < 0xB00000, 1)) {
#else
    if (__builtin_expect(!via_overlay_fast && address < 0x500000, 1)) {
#endif
        uint32_t a = address & rom_mask;
        return ((uint32_t)rom[a] << 24) | ((uint32_t)rom[a+1] << 16) |
               ((uint32_t)rom[a+2] << 8) | rom[a+3];
    }
#if MAC_ROM_LC_II
    if (VRAM_HIT(address, 4)) {
        const uint8_t *v = v8_vram + (address - V8_VRAM_BASE);
        return ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) |
               ((uint32_t)v[2] << 8) | v[3];
    }
#endif
    return (mem_read_byte(address) << 24) |
           (mem_read_byte(address + 1) << 16) |
           (mem_read_byte(address + 2) << 8) |
            mem_read_byte(address + 3);
}

MEM_CB_ATTR void m68k_write_memory_8(unsigned int address, unsigned int value)
{
    /* Fast path: normal mode RAM write — alias via ram_mask.
     * Skip if address is in the watchpoint range and
     * mac_boot_read_bypass is active — those writes need guard checks. */
    address &= 0x00FFFFFF;
#if MAC_ROM_LC_II
#if MAC_DIAG
    if (address >= 0x031C && address <= 0x031F) {
        static int v8mt8_cnt = 0;
        if (v8mt8_cnt < 6) {
            ESP_LOGW(TAG, "V8MT8 #%d: $%04X<-%02X PC=$%06X",
                     v8mt8_cnt++, address, value & 0xFF,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        }
    }
    if (address >= 0x1FFAC0 && address <= 0x1FFAF0) {
        static int v8cs8_cnt = 0;
        if (v8cs8_cnt < 6) {
            ESP_LOGW(TAG, "V8CS8 #%d: $%06X<-%02X PC=$%06X",
                     v8cs8_cnt++, address, value & 0xFF,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        }
    }
    if (address >= 0x360000 && address <= 0x36000F) {
        static int v8cp8_cnt = 0;
        if (v8cp8_cnt < 8) {
            ESP_LOGW(TAG, "V8CPY8 #%d: $%06X<-%02X PC=$%06X",
                     v8cp8_cnt++, address, value & 0xFF,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        }
    }
    if (address >= 0x0FF9F0 && address <= 0x0FF9FF) {
        static int v8stk8 = 0;
        if (v8stk8 < 16) {
            ESP_LOGW(TAG, "V8STK8 #%d: $%06X<-%02X PC=$%06X",
                     v8stk8++, address, value & 0xFF,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        }
    }
    if (address >= 0x100000 && address < 0x100010) {
        static int v8c100_8 = 0;
        if (v8c100_8 < 16) {
            ESP_LOGW(TAG, "V8C100_8 #%d: $%06X<-%02X PC=$%06X",
                     v8c100_8++, address, value & 0xFF,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        }
    }
#endif /* MAC_DIAG */
    if (__builtin_expect(!via_overlay_fast &&
        (address < ram_size), 1)) {
#else
    if (__builtin_expect(!via_overlay_fast && address < 0x400000, 1)) {
#endif
        if (__builtin_expect(!mac_boot_read_bypass || address >= 0x0A20, 1)) {
#if MAC_DIAG
            /* Catch 8-bit writes to SysZone allocPtr ($162E-$1631) — these
             * bypass mem_write_byte watchpoints because address >= $0A20. */
            if (__builtin_expect(mac_boot_read_bypass &&
                address >= 0x162E && address <= 0x1631, 0)) {
                static int ap8_cnt = 0;
                if (ap8_cnt < 20) {
                    ESP_LOGW(TAG, "WP-ALLOC8 #%d: $%04X<-%02X PC=$%06X SP=$%06X",
                             ap8_cnt, address, value & 0xFF,
                             m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF,
                             m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
                    ap8_cnt++;
                }
            }
#endif /* MAC_DIAG */
            ARM_FAST_WR();
            ram[address & ram_mask] = value & 0xFF;
            return;
        }
    }
#if MAC_ROM_LC_II
    if (VRAM_HIT(address, 1)) { v8_vram[address - V8_VRAM_BASE] = (uint8_t)value; return; }
#endif
    mem_write_byte(address, value & 0xFF);
}

MEM_CB_ATTR void m68k_write_memory_16(unsigned int address, unsigned int value)
{
    address &= 0x00FFFFFF;
#if MAC_DIAG
#if MAC_ROM_LC_II
    if (address >= 0x4D4EE0 && address <= 0x4D4EEF) {
        static int v8b = 0;
        if (v8b < 12) {
            ESP_LOGW(TAG, "V8CB16 #%d: $%06X<-%04X PC=$%06X", v8b++, address, value,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        }
    }
    if (address >= 0x0FF9F0 && address <= 0x0FF9FF) {
        static int v8stk16 = 0;
        if (v8stk16 < 16) {
            ESP_LOGW(TAG, "V8STK16 #%d: $%06X<-%04X PC=$%06X",
                     v8stk16++, address, value,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        }
    }
    if (address >= 0x100000 && address < 0x100010) {
        static int v8c100_16 = 0;
        if (v8c100_16 < 16) {
            ESP_LOGW(TAG, "V8C100_16 #%d: $%06X<-%04X PC=$%06X",
                     v8c100_16++, address, value,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        }
    }
#endif
    if (address >= 0x2F00 && address <= 0x3160) {
        static int w2f16_cnt = 0;
        if (w2f16_cnt < 40) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            ESP_LOGW(TAG, "W2F16 #%d: $%04X<-%04X PC=$%06X", w2f16_cnt++, address, value & 0xFFFF, pc);
        }
    }

    /* Monitor 16-bit writes to SysZone header key fields */
    if (__builtin_expect(mac_boot_read_bypass &&
        ((address >= 0x1600 && address <= 0x1603) ||
         (address >= 0x162E && address <= 0x1631)), 0)) {
        static int zh16_cnt = 0;
        if (zh16_cnt < 20) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            ESP_LOGW(TAG, "WP-ZH16 #%d: $%04X<-$%04X PC=$%06X SP=$%06X",
                     zh16_cnt, address, value & 0xFFFF, pc,
                     m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
            zh16_cnt++;
        }
    }

    /* Monitor 16-bit writes near TB[$112] at $1248-$124B */
    if (__builtin_expect(mac_boot_read_bypass &&
        address >= 0x1248 && address <= 0x124A, 0)) {
        static int tb112w16 = 0;
        if (tb112w16 < 5) {
            ESP_LOGW(TAG, "TB[$112] W16 #%d: $%04X<-%04X PC=$%06X",
                     tb112w16, address, value & 0xFFFF,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
            tb112w16++;
        }
    }

#endif /* MAC_DIAG */
    /* Fast path: normal mode RAM write — alias via ram_mask */
#if MAC_ROM_LC_II
#if MAC_DIAG
    if (address >= 0x031C && address <= 0x031F) {
        static int v8mt16_cnt = 0;
        if (v8mt16_cnt < 6) {
            ESP_LOGW(TAG, "V8MT16 #%d: $%04X<-%04X PC=$%06X",
                     v8mt16_cnt++, address, value & 0xFFFF,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        }
    }
    if (address >= 0x1FFAC0 && address <= 0x1FFAF0) {
        static int v8cs16_cnt = 0;
        if (v8cs16_cnt < 6) {
            ESP_LOGW(TAG, "V8CS16 #%d: $%06X<-%04X PC=$%06X",
                     v8cs16_cnt++, address, value & 0xFFFF,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        }
    }
    if (address >= 0x360000 && address <= 0x36000F) {
        static int v8cp16_cnt = 0;
        if (v8cp16_cnt < 8) {
            ESP_LOGW(TAG, "V8CPY16 #%d: $%06X<-%04X PC=$%06X",
                     v8cp16_cnt++, address, value & 0xFFFF,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        }
    }
#endif /* MAC_DIAG */
    if (__builtin_expect(!via_overlay_fast &&
        (address < ram_size), 1)) {
#else
    if (__builtin_expect(!via_overlay_fast && address < 0x400000, 1)) {
#endif
        if (__builtin_expect(!mac_boot_read_bypass || address >= 0x0A20, 1)) {
            uint32_t ra = address & ram_mask;
            ARM_FAST_WR();
            ram[ra]   = (value >> 8) & 0xFF;
            ram[ra+1] = value & 0xFF;
            return;
        }
    }
#if MAC_ROM_LC_II
    if (VRAM_HIT(address, 2)) {
        uint8_t *v = v8_vram + (address - V8_VRAM_BASE);
        v[0] = (uint8_t)(value >> 8); v[1] = (uint8_t)value;
        return;
    }
#endif
    mem_write_byte(address, (value >> 8) & 0xFF);
    mem_write_byte(address + 1, value & 0xFF);
}

MEM_CB_ATTR void m68k_write_memory_32(unsigned int address, unsigned int value)
{
    address &= 0x00FFFFFF;
#if MAC_DIAG
#if MAC_ROM_LC_II
    if (address >= 0x031C && address <= 0x031F) {
        static int v8mt = 0;
        if (v8mt < 10) {
            ESP_LOGW(TAG, "V8MT32 #%d: $031C<-%08X PC=$%06X", v8mt++, value,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        }
    }
    if (address >= 0x5FBFF0 && address <= 0x5FC010) {
        static int v8wc = 0;
        if (v8wc < 16) {
            ESP_LOGW(TAG, "V8WC32 #%d: $%06X<-%08X PC=$%06X", v8wc++, address, value,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        }
    }
    if (address >= 0x400000 && address <= 0x40000F) {
        static int v8cp = 0;
        if (v8cp < 8) {
            ESP_LOGW(TAG, "V8CP32 #%d: $%06X<-%08X PC=$%06X", v8cp++, address, value,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        }
    }
    if (address >= 0x4D4EDC && address <= 0x4D4EEF) {
        static int v8cb = 0;
        if (v8cb < 12) {
            ESP_LOGW(TAG, "V8CB32 #%d: $%06X<-%08X PC=$%06X", v8cb++, address, value,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        }
    }
    if (address >= 0x0FF9F0 && address <= 0x0FF9FF) {
        static int v8stk = 0;
        if (v8stk < 12) {
            ESP_LOGW(TAG, "V8STK32 #%d: $%06X<-%08X PC=$%06X", v8stk++, address, value,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        }
    }
    if (address >= 0x100000 && address < 0x100010) {
        static int v8c100 = 0;
        if (v8c100 < 12) {
            ESP_LOGW(TAG, "V8C100 #%d: $%06X<-%08X PC=$%06X", v8c100++, address, value,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        }
    }
#endif
    if (address >= 0x2F00 && address <= 0x3160) {
        static int w2f32_cnt = 0;
        if (w2f32_cnt < 40) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            ESP_LOGW(TAG, "W2F32 #%d: $%04X<-%08X PC=$%06X", w2f32_cnt++, address, value, pc);
        }
    }

    /* Monitor writes to TB[$112] (_InitWindows) at $1248 */
    if (__builtin_expect(mac_boot_read_bypass && address == 0x001248, 0)) {
        static int tb112_cnt = 0;
        if (tb112_cnt < 5) {
            uint32_t old_val = ((uint32_t)ram[0x1248]<<24)|((uint32_t)ram[0x1249]<<16)|
                               ((uint32_t)ram[0x124A]<<8)|ram[0x124B];
            ESP_LOGW(TAG, "TB[$112] WRITE #%d: $%08lX -> $%08lX PC=$%06X",
                     tb112_cnt, (unsigned long)old_val, (unsigned long)value,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
            tb112_cnt++;
        }
    }

    /* Monitor 32-bit writes to SysZone header fields (above $0A20 fast-path
     * threshold, so mem_write_byte watchpoints don't fire for these). */
    if (__builtin_expect(mac_boot_read_bypass &&
        (address == 0x1600 || address == 0x162C || address == 0x162E), 0)) {
        static int zh_cnt = 0;
        if (zh_cnt < 30) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            const char *field = (address == 0x1600) ? "bkLim" :
                                (address == 0x162E) ? "allocPtr" : "sparePtr";
            uint32_t old = ((uint32_t)ram[address]<<24)|((uint32_t)ram[address+1]<<16)|
                           ((uint32_t)ram[address+2]<<8)|ram[address+3];
            ESP_LOGW(TAG, "WP-ZH32 #%d: %s($%04X) $%08lX->$%08lX PC=$%06X SP=$%06X",
                     zh_cnt, field, address, (unsigned long)old, (unsigned long)value,
                     pc, m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF);
            zh_cnt++;
        }
    }

#endif /* MAC_DIAG */
    /* Fast path: normal mode RAM write — alias via ram_mask */
#if MAC_ROM_LC_II
#if MAC_DIAG
    if (address >= 0x031C && address <= 0x031F) {
        static int v8mt32_cnt = 0;
        if (v8mt32_cnt < 6) {
            ESP_LOGW(TAG, "V8MT32 #%d: $%04X<-%08X PC=$%06X",
                     v8mt32_cnt++, address, value,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        }
    }
    if (address >= 0x1FFAC0 && address <= 0x1FFAF0) {
        static int v8cs32_cnt = 0;
        if (v8cs32_cnt < 6) {
            ESP_LOGW(TAG, "V8CS32 #%d: $%06X<-%08X PC=$%06X",
                     v8cs32_cnt++, address, value,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        }
    }
    if (address >= 0x360000 && address <= 0x36000F) {
        static int v8cp32_cnt = 0;
        if (v8cp32_cnt < 8) {
            ESP_LOGW(TAG, "V8CPY32 #%d: $%06X<-%08X PC=$%06X",
                     v8cp32_cnt++, address, value,
                     m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF);
        }
    }
#endif /* MAC_DIAG */
    if (__builtin_expect(!via_overlay_fast &&
        (address < ram_size), 1)) {
#else
    if (__builtin_expect(!via_overlay_fast && address < 0x400000, 1)) {
#endif
        if (__builtin_expect(!mac_boot_read_bypass || address >= 0x0A20, 1)) {
            uint32_t ra = address & ram_mask;
            ARM_FAST_WR();
            if (__builtin_expect(ra + 3 < ram_size, 1)) {
                ram[ra]   = (value >> 24) & 0xFF;
                ram[ra+1] = (value >> 16) & 0xFF;
                ram[ra+2] = (value >> 8) & 0xFF;
                ram[ra+3] = value & 0xFF;
            } else {
                /* Rare: access spans aliasing boundary */
                ram[ra] = (value >> 24) & 0xFF;
                ram[(address+1) & ram_mask] = (value >> 16) & 0xFF;
                ram[(address+2) & ram_mask] = (value >> 8) & 0xFF;
                ram[(address+3) & ram_mask] = value & 0xFF;
            }
            return;
        }
    }
#if MAC_ROM_LC_II
    if (VRAM_HIT(address, 4)) {
        uint8_t *v = v8_vram + (address - V8_VRAM_BASE);
        v[0] = (uint8_t)(value >> 24); v[1] = (uint8_t)(value >> 16);
        v[2] = (uint8_t)(value >> 8);  v[3] = (uint8_t)value;
        return;
    }
#endif
    mem_write_byte(address, (value >> 24) & 0xFF);
    mem_write_byte(address + 1, (value >> 16) & 0xFF);
    mem_write_byte(address + 2, (value >> 8) & 0xFF);
    mem_write_byte(address + 3, value & 0xFF);
}

/* Disassembler reads - same as normal reads */
unsigned int m68k_read_disassembler_8(unsigned int address)  { return m68k_read_memory_8(address); }
unsigned int m68k_read_disassembler_16(unsigned int address) { return m68k_read_memory_16(address); }
unsigned int m68k_read_disassembler_32(unsigned int address) { return m68k_read_memory_32(address); }

/* ====================================================================== */
/* === SEPARATE INSTRUCTION FETCH (M68K_SEPARATE_READS optimization) ==== */
/* ====================================================================== */
/*
 * With M68K_SEPARATE_READS = OPT_ON, Musashi calls dedicated functions
 * for instruction/immediate fetches and PC-relative reads, separate from
 * data memory reads. This gives us a lean hot path for instruction fetches
 * (no diagnostic watchpoints, minimal ROM intercepts), while the full
 * intercept logic remains in the data-read path (m68k_read_memory_*).
 *
 * During boot (!mac_post_boot), all reads fall through to the full
 * m68k_read_memory_* functions for complete intercept coverage.
 * After boot, the lean path handles 99%+ of fetches with just
 * RAM/ROM dispatch + essential functional patches.
 *
 * IRAM_ATTR places these in internal SRAM for guaranteed fast access,
 * bypassing the flash instruction cache entirely.
 */

IRAM_ATTR unsigned int m68k_read_immediate_16(unsigned int address)
{
    address &= 0x00FFFFFF;

    /* During boot: use full read path with all intercepts */
    if (__builtin_expect(!mac_post_boot, 0)) {
        return m68k_read_memory_16(address);
    }

    /* Post-boot: lean RAM fast path */
#if MAC_ROM_LC_II
    if (__builtin_expect(address < ram_size, 1)) {
        uint32_t ra = address & ram_mask;
        ARM_FAST_FETCH();
        return (ram[ra] << 8) | ram[ra + 1];
    }
#else
    if (__builtin_expect(address < 0x400000, 1)) {
        uint32_t ra = address & ram_mask;
        ARM_FAST_FETCH();
        return (ram[ra] << 8) | ram[ra + 1];
    }
#endif

    /* Post-boot: lean ROM fast path with essential patches only.
     * Only functional intercepts that affect Finder operation are kept.
     * Diagnostic logging (_InitGraf, _InitFonts, etc.) is omitted. */
#if MAC_ROM_LC_II
    if (__builtin_expect(address >= 0xA00000 && address < 0xB00000, 1)) {
#else
    if (__builtin_expect(address < 0x500000, 1)) {
#endif
#if !MAC_ROM_LC_II
        /* UNIMP trap handler: return safe stub instead of system error */
        if (__builtin_expect(address == 0x400768, 0)) return 0x91C8; /* SUBA.L A0,A0 */
        if (__builtin_expect(address == 0x40076A, 0)) return 0x7000; /* MOVEQ #0,D0 */
        if (__builtin_expect(address == 0x40076C, 0)) return 0x4E75; /* RTS */
#endif
        /* Device Manager intercepts (Finder disk I/O) */
#if !MAC_ROM_LC_II
        if (__builtin_expect(address == DM_READ_ENTRY, 0)) return read16_dm_read();
        if (__builtin_expect(address == DM_SPIN_LOOP, 0)) return read16_dm_spin_rescue();
        if (__builtin_expect(address == DM_CTL_ENTRY, 0)) return read16_dm_ctl();
        if (__builtin_expect(address == DM_STAT_ENTRY, 0)) return read16_dm_stat();
#if DISK_WRITABLE
        if (__builtin_expect(address == DM_WRITE_ENTRY, 0)) return read16_dm_write(); /* writable disk */
#endif
#endif
#if !MAC_ROM_LC_II
        /* .SONY driver intercepts */
        if (__builtin_expect(address == 0x434B76, 0)) return read16_prime();
        if (__builtin_expect(address == 0x434BAC, 0)) return read16_sony();
#endif
        /* Return ROM data directly — no diagnostic logging */
        uint32_t a = address & rom_mask;
        uint16_t w = (rom[a] << 8) | rom[a + 1];
        CLASSIC_NATIVE_PATCH_LEAN(a, w);
        return w;
    }

    /* Peripheral space (shouldn't happen for instruction fetch) */
    return m68k_read_memory_16(address);
}

IRAM_ATTR unsigned int m68k_read_immediate_32(unsigned int address)
{
    address &= 0x00FFFFFF;

    if (__builtin_expect(!mac_post_boot, 0)) {
#if !MAC_ROM_LC_II && CLASSIC_NATIVE_LCD
        uint32_t v = m68k_read_memory_32(address);
        if (address >= 0x400000 && address < 0x500000)
            CLASSIC_NATIVE_PATCH32(address & rom_mask, v);
        return v;
#else
        return m68k_read_memory_32(address);
#endif
    }

    /* Post-boot: lean RAM read */
#if MAC_ROM_LC_II
    if (__builtin_expect(address < ram_size, 1)) {
#else
    if (__builtin_expect(address < 0x400000, 1)) {
#endif
        uint32_t ra = address & ram_mask;
        return ((uint32_t)ram[ra] << 24) | ((uint32_t)ram[ra+1] << 16) |
               ((uint32_t)ram[ra+2] << 8) | ram[ra+3];
    }

    /* Post-boot: lean ROM read (no intercepts for 32-bit immediate data) */
#if MAC_ROM_LC_II
    if (__builtin_expect(address >= 0xA00000 && address < 0xB00000, 1)) {
#else
    if (__builtin_expect(address < 0x500000, 1)) {
#endif
        uint32_t a = address & rom_mask;
        uint32_t v = ((uint32_t)rom[a] << 24) | ((uint32_t)rom[a+1] << 16) |
                     ((uint32_t)rom[a+2] << 8) | rom[a+3];
        CLASSIC_NATIVE_PATCH32(a, v);
        return v;
    }

    return m68k_read_memory_32(address);
}

IRAM_ATTR unsigned int m68k_read_pcrelative_8(unsigned int address)
{
    address &= 0x00FFFFFF;

    if (__builtin_expect(!mac_post_boot, 0)) {
        return m68k_read_memory_8(address);
    }

#if MAC_ROM_LC_II
    if (__builtin_expect(address < ram_size, 1)) {
        return ram[address & ram_mask];
    }
    if (__builtin_expect(address >= 0xA00000 && address < 0xB00000, 1)) {
        return rom[address & rom_mask];
    }
#else
    if (__builtin_expect(address < 0x400000, 1)) {
        return ram[address & ram_mask];
    }
    if (__builtin_expect(address < 0x500000, 1)) {
        return rom[address & rom_mask];
    }
#endif

    return m68k_read_memory_8(address);
}

IRAM_ATTR unsigned int m68k_read_pcrelative_16(unsigned int address)
{
    address &= 0x00FFFFFF;

    if (__builtin_expect(!mac_post_boot, 0)) {
        return m68k_read_memory_16(address);
    }

#if MAC_ROM_LC_II
    if (__builtin_expect(address < ram_size, 1)) {
#else
    if (__builtin_expect(address < 0x400000, 1)) {
#endif
        uint32_t ra = address & ram_mask;
        return (ram[ra] << 8) | ram[ra + 1];
    }
#if MAC_ROM_LC_II
    if (__builtin_expect(address >= 0xA00000 && address < 0xB00000, 1)) {
#else
    if (__builtin_expect(address < 0x500000, 1)) {
#endif
        uint32_t a = address & rom_mask;
        return (rom[a] << 8) | rom[a + 1];
    }

    return m68k_read_memory_16(address);
}

IRAM_ATTR unsigned int m68k_read_pcrelative_32(unsigned int address)
{
    address &= 0x00FFFFFF;

    if (__builtin_expect(!mac_post_boot, 0)) {
        return m68k_read_memory_32(address);
    }

#if MAC_ROM_LC_II
    if (__builtin_expect(address < ram_size, 1)) {
#else
    if (__builtin_expect(address < 0x400000, 1)) {
#endif
        uint32_t ra = address & ram_mask;
        return ((uint32_t)ram[ra] << 24) | ((uint32_t)ram[ra+1] << 16) |
               ((uint32_t)ram[ra+2] << 8) | ram[ra+3];
    }
#if MAC_ROM_LC_II
    if (__builtin_expect(address >= 0xA00000 && address < 0xB00000, 1)) {
#else
    if (__builtin_expect(address < 0x500000, 1)) {
#endif
        uint32_t a = address & rom_mask;
        return ((uint32_t)rom[a] << 24) | ((uint32_t)rom[a+1] << 16) |
               ((uint32_t)rom[a+2] << 8) | rom[a+3];
    }

    return m68k_read_memory_32(address);
}

/* Interrupt acknowledge - Mac uses autovectored interrupts */
int mac_int_ack_handler(int level)
{
    (void)level;
#if MAC_IDLE_FF
    mac_idle_note_irq();
#endif
    return M68K_INT_ACK_AUTOVECTOR;
}

/* RESET instruction handler - reset peripherals */
void mac_reset_handler(void)
{
    via_reset();
    scc_init();
    ESP_LOGI(TAG, "CPU RESET instruction executed");
#if !MAC_ROM_LC_II
    /* Classic Restart (Finder Special > Restart, or Restart on the "safe to
     * switch off" screen): the ROM executes RESET and re-enters its start
     * code. A warm re-boot cannot work here because the boot bypasses are
     * keyed to frame counts and one-shot state (it ends in Sad Mac
     * 00000001/0000FFFF), so restart the whole board instead: a cold boot
     * of the emulator, like power-cycling the Mac. RESET is not executed
     * during a normal boot, only after mac_post_boot. */
    if (mac_post_boot) {
        ESP_LOGW(TAG, "RESTART: Mac executed RESET after boot -> esp_restart()");
        esp_restart();
    }
#endif
}
