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

/* Embedded ROM image (linked from flash via CMake EMBED_FILES) */
extern const uint8_t rom_start[] asm("_binary_Classic_ROM_start");
extern const uint8_t rom_end[]   asm("_binary_Classic_ROM_end");

/* Disk image - only available if HAS_DISK_IMAGE is set in CMakeLists.txt */
#if HAS_DISK_IMAGE
extern const uint8_t disk_start[] asm("_binary_OS_750_boot_dsk_start");
extern const uint8_t disk_end[]   asm("_binary_OS_750_boot_dsk_end");
#endif

/* Mouse state from touch input */
static volatile int mouse_x = MAC_SCREEN_WIDTH / 2;
static volatile int mouse_y = MAC_SCREEN_HEIGHT / 2;
static volatile bool mouse_button = false;

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

    /* Initialize Musashi 68000 CPU */
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();

    ESP_LOGI(TAG, "CPU initialized, PC=0x%08X, SP=0x%08X",
             m68k_get_reg(NULL, M68K_REG_PC),
             m68k_get_reg(NULL, M68K_REG_SP));

    return true;
}

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
    const int normal_slice = 1000;
    const int fast_slice = 4000;
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

    while (1) {
        /* Execute one frame's worth of CPU cycles */
        int cycles_remaining = MAC_CYCLES_PER_VBLANK;
        int total_executed = 0;
        bool vblank_fired = false;
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
            prof_exec_sum += (t_exec1 - t_exec0);
            cycles_remaining -= executed;
            total_executed += executed;

            /* Read PC once per slice — shared by all post-execute checks.
             * Eliminates 7 redundant m68k_get_reg calls per slice. */
            uint32_t pc_slice = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;

            /* === POST-EXECUTE PC VALIDATION ===
             * Catches runaway PC before it cascades into unrecoverable state. */
            {
                static bool crash_detected = false;
                if (!crash_detected) {
                    /* Valid ranges: RAM $0-$1FFFFF, ROM $400000-$47FFFF */
                    if ((pc_slice >= 0x200000 && pc_slice < 0x400000) ||
                        pc_slice >= 0x480000) {
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
                        /* Recovery: redirect to tight loop */
                        if (cr) {
                            m68k_set_reg(M68K_REG_SP, 0x0FFA00);
                            m68k_set_reg(M68K_REG_PC, 0x000036);
                            ESP_LOGW(TAG, "CRASH-RECOVERY: PC=$0036 SP=$0FFA00");
                        }
                    }
                }
            }

            /* Fire VBlank mid-frame so ROM dispatch loop can see CA1 in IFR
             * even if it reads ORA (which clears CA1) earlier in the loop */
            if (!vblank_fired && total_executed >= MAC_CYCLES_PER_VBLANK / 2) {
                via_vblank();
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
                if (pc_slice == 0x401CF0 || pc_slice == 0x401D80 || pc_slice == 0x401D96) {
                    uint32_t old_d6 = m68k_get_reg(NULL, M68K_REG_D6);
                    if (old_d6 != 0) {
                        m68k_set_reg(M68K_REG_D6, 0);
                        ESP_LOGW(TAG, "BYPASS: Cleared D6 at PC=%06X F%d (was 0x%08X)",
                                 pc_slice, frame_count, old_d6);
                    }
                }
            }

            /* Log when normal boot entry is reached */
            if (!boot_logged) {
                if (pc_slice == 0x401092) {
                    boot_logged = true;
                    ESP_LOGI(TAG, "BOOT: Normal boot entry at F%d", frame_count);
                }
            }

            /* SCSI polling bypass: ROM at 41A44A polls SCSI BSY in a tight loop.
             * With no SCSI device, clear loop counters to exit quickly. */
            {
                if (pc_slice == 0x41A44A || pc_slice == 0x41A44E || pc_slice == 0x41A452) {
                    m68k_set_reg(M68K_REG_D1, 0);
                    m68k_set_reg(M68K_REG_D5, 0);
                }
            }

            /* Boot _Read bypass: ROM-level NOP at $400CAC handled in
             * mac_memory.c via mac_boot_read_bypass flag. */

            /* I/O completion bypass: the ROM's I/O Manager polls ioResult
             * at $404332 after issuing a synchronous I/O call. Since our
             * SWIM driver can't complete sector reads, fulfill the request
             * directly from the disk image and set ioResult=0. */
            if (mac_boot_read_bypass) {
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
                                memcpy(rram + buf_m, disk + ioPosOffset, ioReqCount);
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
                    else if (pcd >= 0x100000 && pcd < 0x200000) {
                        static int bb_log = 0;
                        static uint32_t last_bb_pc = 0;
                        if (bb_log < 40) {
                            ESP_LOGW(TAG, "BB PC=%06X SP=%06X F%d",
                                     pcd, m68k_get_reg(NULL, M68K_REG_SP) & 0xFFFFFF,
                                     frame_count);
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
                            if (!crash_recovered && rram) {
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
            via_tick(executed);

            /* Check for VIA interrupt -> raise CPU IRQ level 1 */
            if (via_irq_pending()) {
                m68k_set_irq(M68K_IRQ_1);
            } else {
                m68k_set_irq(M68K_IRQ_NONE);
            }
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
            }
        }
        if (frame_count == 600) {
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
                    ram_ptr[0x0042]=0x00;                       /* writeProt=0 */
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

        frame_count++;

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
        }

        /* Write mouse state to Mac low-memory globals once per frame.
         * Mac Point format: big-endian, 4 bytes = (Y.hi Y.lo X.hi X.lo).
         * MBState bit 7: 0 = button down, 1 = button up. */
        if (mac_post_boot) {
            uint8_t *r = mac_mem_get_ram();
            if (r) {
                int mx = mouse_x;
                int my = mouse_y;
                bool mb = mouse_button;

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

                /* Mouse $0830 — current mouse position (normally ROM copies
                 * from RawMouse, but we set it directly for responsiveness) */
                r[0x0830] = (my >> 8) & 0xFF;
                r[0x0831] = my & 0xFF;
                r[0x0832] = (mx >> 8) & 0xFF;
                r[0x0833] = mx & 0xFF;

                /* CrsrNew $08CE — set to $FF to trigger cursor redraw */
                r[0x08CE] = 0xFF;

                /* CrsrCouple $08CF — must be $FF for cursor to track mouse */
                r[0x08CF] = 0xFF;

                /* MBState $0172 — bit 7: 0=down, 1=up */
                if (mb) {
                    r[0x0172] &= ~0x80;  /* button down: clear bit 7 */
                } else {
                    r[0x0172] |= 0x80;   /* button up: set bit 7 */
                }
            }
        }

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

        /* Touch input: trackpad-style relative movement.
         * First touch sets button down but doesn't move cursor.
         * Swiping moves cursor by the delta. Release sets button up. */
        {
            static int64_t last_touch_us = 0;
            static bool was_touching = false;
            static int last_tx = 0, last_ty = 0;
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_touch_us > 16000) {  /* ~60Hz */
                last_touch_us = now_us;
                int tx, ty;
                bool touch = display_get_touch(&tx, &ty);
                if (touch) {
                    if (was_touching) {
                        /* Continued touch: move cursor by delta */
                        int dx = -(ty - last_ty);  /* inverted horizontal */
                        int dy = tx - last_tx;
                        if (dx != 0 || dy != 0) {
                            mac_emu_set_mouse(
                                mouse_x + dx,
                                mouse_y + dy,
                                true
                            );
                        }
                    }
                    /* First touch or continued: record position, set button */
                    mouse_button = true;
                    was_touching = true;
                    last_tx = tx;
                    last_ty = ty;
                } else {
                    mouse_button = false;
                    was_touching = false;
                }
            }
        }

        /* No frame rate limiter — run at maximum speed.
         * Yield periodically to prevent watchdog timeout. */
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
    if (x >= MAC_SCREEN_WIDTH) x = MAC_SCREEN_WIDTH - 1;
    if (y < 0) y = 0;
    if (y >= MAC_SCREEN_HEIGHT) y = MAC_SCREEN_HEIGHT - 1;

    mouse_x = x;
    mouse_y = y;
    mouse_button = button;
}
