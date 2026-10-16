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
#include "mac_iwm.h"
#include "m68k.h"

#include <string.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_attr.h>

static const char *TAG = "mac_mem";
int initgraf_seen = 0;  /* Set when _InitGraf ROM entry is accessed */

/* Memory arrays */
static uint8_t *ram = NULL;         /* Main RAM (in PSRAM) */
static const uint8_t *rom = NULL;   /* ROM image (in flash) */
static uint32_t ram_size = 0;
static uint32_t rom_size = 0;
static uint32_t rom_mask = 0;       /* For mirroring: rom_size - 1 */
static uint32_t ram_mask = 0;       /* For mirroring: ram_size - 1 */


/* Boot bypass flag — set by mac_emulator.c when disk is ready */
volatile bool mac_boot_read_bypass = false;

/* Post-boot mode — set after Finder is running. Enables lean instruction
 * fetch path that skips diagnostic watchpoints and logging intercepts. */
bool mac_post_boot = false;

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

    ESP_LOGI(TAG, "Memory initialized: %luKB RAM (PSRAM), %luKB ROM (flash)",
             (unsigned long)(ram_size / 1024), (unsigned long)(rom_size / 1024));

    /* Initialize peripherals */
    via_init();
    scc_init();
    iwm_init(disk_data, disk_size);

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

    /* SCSI NCR 5380: 0x580000-0x5FFFFF
     * Return 0x00 for all registers = bus free, no devices.
     * Without this stub, reads return 0xFF (all signals asserted),
     * which looks like a stuck SCSI bus and confuses the ROM. */
    if (address >= 0x580000 && address < 0x600000) {
        return 0x00;
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

    /* Writes to unmapped space are silently ignored */
}

/* ====================================================================== */
/* =================== MUSASHI REQUIRED CALLBACKS ======================= */
/* ====================================================================== */

unsigned int m68k_read_memory_8(unsigned int address)
{
    /* Fast path: normal mode RAM read (most common case).
     * Alias via ram_mask — matches real Mac hardware behavior. */
    address &= 0x00FFFFFF;
    if (__builtin_expect(!via_overlay_fast && address < 0x400000, 1)) {
        return ram[address & ram_mask];
    }
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
                memcpy(ram + buf_m, disk + io_off, io_cnt);
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
            ram[a0m+0x0C] = 0; ram[a0m+0x0D] = 0;
            ram[a0m+0x0E] = 0; ram[a0m+0x0F] = 0;
            ram[a0m+0x10] = 0; ram[a0m+0x11] = 0;
            m68k_set_reg(M68K_REG_D0, 0);
            dm_cnt++;
            return 0x4E75;  /* RTS */
        }
    }
    return rom_word_at(0x40315C);
}

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
            ram[a0m+0x0C] = 0; ram[a0m+0x0D] = 0;
            ram[a0m+0x0E] = 0; ram[a0m+0x0F] = 0;
            ram[a0m+0x10] = 0; ram[a0m+0x11] = 0;
            m68k_set_reg(M68K_REG_D0, 0);
            ctl_cnt++;
            return 0x4E75;  /* RTS */
        }
    }
    return rom_word_at(0x4031E0);
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
            ram[a0m+0x0C] = 0; ram[a0m+0x0D] = 0;
            ram[a0m+0x0E] = 0; ram[a0m+0x0F] = 0;
            ram[a0m+0x10] = 0; ram[a0m+0x11] = 0;
            m68k_set_reg(M68K_REG_D0, 0);
            stat_cnt++;
            return 0x4E75;  /* RTS */
        }
    }
    return rom_word_at(0x4031EE);
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
        memcpy(ram + buf_m, disk + io_off, io_cnt);
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
    return rom_word_at(0x400E0A);
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
            memcpy(ram + buf_m, disk + io_off, io_cnt);
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
            memcpy(ram + buf_m, disk + io_off, io_cnt);
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

unsigned int m68k_read_memory_16(unsigned int address)
{
    address &= 0x00FFFFFF;

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
     * runs normally and handles sizing. Mini vMac lets it run. */
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

    /* Fast path: normal mode RAM read — by far the most common case.
     * Skips all intercept checks for addresses below $400000.
     * Alias via ram_mask (power-of-2) so the ROM's sizing code
     * sees correct hardware-style aliasing at the RAM boundary. */
    if (__builtin_expect(!via_overlay_fast && address < 0x400000, 1)) {
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
        uint32_t ra = address & ram_mask;
        if (__builtin_expect(ra + 1 < ram_size, 1)) {
            return (ram[ra] << 8) | ram[ra + 1];
        }
        /* Rare: access spans aliasing boundary */
        return (ram[ra] << 8) | ram[(address + 1) & ram_mask];
    }

    /* Normal mode ROM read ($400000-$4FFFFF).
     * Check intercepts only for specific addresses via if-chain.
     * For the vast majority of ROM reads (instruction fetches), no
     * intercept matches and we return ROM data directly.
     * The old approach checked all intercepts on every 16-bit read;
     * this limits intercept checks to ROM-range reads only. */
    if (__builtin_expect(!via_overlay_fast && address >= 0x400000 && address < 0x500000, 1)) {
        if (__builtin_expect(mac_boot_read_bypass, 0)) {
            if (address == 0x400CAC) return 0x4E71; /* BEQ→NOP */

            /* CRITICAL FIX: Patch UNIMP trap handler to return safely.
             * The ROM's handler at $400768 does MOVEQ #12,D0; BRA $40138A
             * which jumps to the SYSTEM ERROR handler (crash dialog).
             * When ptch resource code calls a not-yet-patched trap, this
             * kills the boot process.  Replace with:
             *   SUBA.L A0,A0  ; A0 = NULL (safe handle)
             *   MOVEQ  #0,D0  ; no error
             *   RTS            ; return to caller */
            if (address == 0x400768) return 0x91C8;  /* SUBA.L A0,A0 */
            if (address == 0x40076A) return 0x7000;  /* MOVEQ #0, D0 */
            if (address == 0x40076C) return 0x4E75;  /* RTS */

            if (address == 0x40315C) return read16_dm_read();
            if (address == 0x4031E0) return read16_dm_ctl();
            if (address == 0x4031EE) return read16_dm_stat();
            if (address == 0x400DF4) read16_sub6_patch();  /* patches PB, falls through */
            if (address == 0x400E0A) return read16_read_trap();
            if (address == 0x434B76) return read16_prime();
            if (address == 0x434BAC) return read16_sony();

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
            /* (Line-A trap logger removed — exception frame reading was unreliable) */

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
        }

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

        /* Non-intercepted ROM read — return data directly */
        uint32_t a = address & rom_mask;
        return (rom[a] << 8) | rom[a + 1];
    }

    /* Slow path: overlay mode, peripherals, exception handler logging */
    return (mem_read_byte(address) << 8) | mem_read_byte(address + 1);
}

unsigned int m68k_read_memory_32(unsigned int address)
{
    address &= 0x00FFFFFF;
    /* Fast path: normal mode RAM read — alias via ram_mask */
    if (__builtin_expect(!via_overlay_fast && address < 0x400000, 1)) {
        uint32_t ra = address & ram_mask;
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
    if (__builtin_expect(!via_overlay_fast && address < 0x500000, 1)) {
        uint32_t a = address & rom_mask;
        return ((uint32_t)rom[a] << 24) | ((uint32_t)rom[a+1] << 16) |
               ((uint32_t)rom[a+2] << 8) | rom[a+3];
    }
    return (mem_read_byte(address) << 24) |
           (mem_read_byte(address + 1) << 16) |
           (mem_read_byte(address + 2) << 8) |
            mem_read_byte(address + 3);
}

void m68k_write_memory_8(unsigned int address, unsigned int value)
{
    /* Fast path: normal mode RAM write — alias via ram_mask.
     * Skip if address is in the watchpoint range and
     * mac_boot_read_bypass is active — those writes need guard checks. */
    address &= 0x00FFFFFF;
    if (__builtin_expect(!via_overlay_fast && address < 0x400000, 1)) {
        if (__builtin_expect(!mac_boot_read_bypass || address >= 0x0A20, 1)) {
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
            ram[address & ram_mask] = value & 0xFF;
            return;
        }
    }
    mem_write_byte(address, value & 0xFF);
}

void m68k_write_memory_16(unsigned int address, unsigned int value)
{
    address &= 0x00FFFFFF;

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

    /* Fast path: normal mode RAM write — alias via ram_mask */
    if (__builtin_expect(!via_overlay_fast && address < 0x400000, 1)) {
        if (__builtin_expect(!mac_boot_read_bypass || address >= 0x0A20, 1)) {
            uint32_t ra = address & ram_mask;
            ram[ra]   = (value >> 8) & 0xFF;
            ram[ra+1] = value & 0xFF;
            return;
        }
    }
    mem_write_byte(address, (value >> 8) & 0xFF);
    mem_write_byte(address + 1, value & 0xFF);
}

void m68k_write_memory_32(unsigned int address, unsigned int value)
{
    address &= 0x00FFFFFF;

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

    /* Fast path: normal mode RAM write — alias via ram_mask */
    if (__builtin_expect(!via_overlay_fast && address < 0x400000, 1)) {
        if (__builtin_expect(!mac_boot_read_bypass || address >= 0x0A20, 1)) {
            uint32_t ra = address & ram_mask;
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
    if (__builtin_expect(address < 0x400000, 1)) {
        uint32_t ra = address & ram_mask;
        return (ram[ra] << 8) | ram[ra + 1];
    }

    /* Post-boot: lean ROM fast path with essential patches only.
     * Only functional intercepts that affect Finder operation are kept.
     * Diagnostic logging (_InitGraf, _InitFonts, etc.) is omitted. */
    if (__builtin_expect(address < 0x500000, 1)) {
        /* UNIMP trap handler: return safe stub instead of system error */
        if (__builtin_expect(address == 0x400768, 0)) return 0x91C8; /* SUBA.L A0,A0 */
        if (__builtin_expect(address == 0x40076A, 0)) return 0x7000; /* MOVEQ #0,D0 */
        if (__builtin_expect(address == 0x40076C, 0)) return 0x4E75; /* RTS */
        /* Device Manager intercepts (Finder disk I/O) */
        if (__builtin_expect(address == 0x40315C, 0)) return read16_dm_read();
        if (__builtin_expect(address == 0x4031E0, 0)) return read16_dm_ctl();
        if (__builtin_expect(address == 0x4031EE, 0)) return read16_dm_stat();
        /* .SONY driver intercepts */
        if (__builtin_expect(address == 0x434B76, 0)) return read16_prime();
        if (__builtin_expect(address == 0x434BAC, 0)) return read16_sony();
        /* Return ROM data directly — no diagnostic logging */
        uint32_t a = address & rom_mask;
        return (rom[a] << 8) | rom[a + 1];
    }

    /* Peripheral space (shouldn't happen for instruction fetch) */
    return m68k_read_memory_16(address);
}

IRAM_ATTR unsigned int m68k_read_immediate_32(unsigned int address)
{
    address &= 0x00FFFFFF;

    if (__builtin_expect(!mac_post_boot, 0)) {
        return m68k_read_memory_32(address);
    }

    /* Post-boot: lean RAM read */
    if (__builtin_expect(address < 0x400000, 1)) {
        uint32_t ra = address & ram_mask;
        return ((uint32_t)ram[ra] << 24) | ((uint32_t)ram[ra+1] << 16) |
               ((uint32_t)ram[ra+2] << 8) | ram[ra+3];
    }

    /* Post-boot: lean ROM read (no intercepts for 32-bit immediate data) */
    if (__builtin_expect(address < 0x500000, 1)) {
        uint32_t a = address & rom_mask;
        return ((uint32_t)rom[a] << 24) | ((uint32_t)rom[a+1] << 16) |
               ((uint32_t)rom[a+2] << 8) | rom[a+3];
    }

    return m68k_read_memory_32(address);
}

IRAM_ATTR unsigned int m68k_read_pcrelative_8(unsigned int address)
{
    address &= 0x00FFFFFF;

    if (__builtin_expect(!mac_post_boot, 0)) {
        return m68k_read_memory_8(address);
    }

    if (__builtin_expect(address < 0x400000, 1)) {
        return ram[address & ram_mask];
    }
    if (__builtin_expect(address < 0x500000, 1)) {
        return rom[address & rom_mask];
    }

    return m68k_read_memory_8(address);
}

IRAM_ATTR unsigned int m68k_read_pcrelative_16(unsigned int address)
{
    address &= 0x00FFFFFF;

    if (__builtin_expect(!mac_post_boot, 0)) {
        return m68k_read_memory_16(address);
    }

    if (__builtin_expect(address < 0x400000, 1)) {
        uint32_t ra = address & ram_mask;
        return (ram[ra] << 8) | ram[ra + 1];
    }
    if (__builtin_expect(address < 0x500000, 1)) {
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

    if (__builtin_expect(address < 0x400000, 1)) {
        uint32_t ra = address & ram_mask;
        return ((uint32_t)ram[ra] << 24) | ((uint32_t)ram[ra+1] << 16) |
               ((uint32_t)ram[ra+2] << 8) | ram[ra+3];
    }
    if (__builtin_expect(address < 0x500000, 1)) {
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
    return M68K_INT_ACK_AUTOVECTOR;
}

/* RESET instruction handler - reset peripherals */
void mac_reset_handler(void)
{
    via_reset();
    scc_init();
    ESP_LOGI(TAG, "CPU RESET instruction executed");
}
