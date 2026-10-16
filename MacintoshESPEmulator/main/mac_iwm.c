/*
 * IWM/SWIM Emulation for Macintosh Classic
 *
 * The Mac Classic uses the SWIM chip which is backwards-compatible with IWM.
 * For 1.44MB HD disks, the SWIM operates in ISM mode (MFM format).
 *
 * Disk image format: raw 1.44MB (80 tracks * 2 sides * 18 sectors * 512 bytes).
 * Sectors are numbered 1-18 in MFM format.
 *
 * The IWM has several internal state registers controlled by
 * accessing specific addresses. Each address line A0-A3 controls
 * a different state bit (CA0, CA1, CA2, LSTRB, etc.)
 *
 * SWIM ISM mode: after the ROM switches the SWIM to ISM mode,
 * registers are addressed by A2-A0 (3 bits = 8 registers):
 *   0: Data        1: Mark         2: CRC
 *   3: Param data  4: Phase        5: Setup
 *   6: Mode0/Stat0 7: Handshake/Mode1
 */
#include "mac_iwm.h"
#include "mac_memory.h"
#include "m68k.h"
#include <stdio.h>
#include <string.h>
#include <esp_log.h>

static const char *TAG = "iwm";

/* IWM register lines (active in IWM mode) */
#define IWM_CA0     0x01
#define IWM_CA1     0x02
#define IWM_CA2     0x04
#define IWM_LSTRB   0x08
#define IWM_ENABLE  0x10
#define IWM_SELECT  0x20
#define IWM_Q6      0x40
#define IWM_Q7      0x80

/* MFM sector geometry for 1.44MB HD */
#define MFM_SECTORS_PER_TRACK   18
#define MFM_SECTOR_SIZE         512
#define MFM_SIDES               2
#define MFM_TRACKS              80

/* SWIM ISM register indices (from A2-A0) */
#define ISM_DATA        0
#define ISM_MARK        1
#define ISM_CRC         2
#define ISM_PARAM       3
#define ISM_PHASE       4
#define ISM_SETUP       5
#define ISM_MODE0       6   /* Read: status0, Write: mode0 */
#define ISM_HANDSHAKE   7   /* Read: handshake, Write: mode1 */

/* ISM handshake register bits */
#define ISM_HSHK_RDDATA     0x80    /* Read data register has data */
#define ISM_HSHK_SENSE      0x20    /* Sense input (FCLK for HD) */
#define ISM_HSHK_MOTORON    0x10    /* Motor is on */
#define ISM_HSHK_ERROR      0x08    /* CRC error */

/* ISM setup register bits */
#define ISM_SETUP_GCRMFM    0x04    /* 0=GCR, 1=MFM */
#define ISM_SETUP_CLKSPD    0x02    /* 0=slow, 1=fast (16MHz for MFM) */

/* Sector stream format for MFM reads */
#define SECTOR_HEADER_SIZE  (12 + 3 + 1 + 4 + 2 + 22 + 12 + 3 + 1)  /* 60 bytes before data */
#define SECTOR_TRAILER_SIZE 2   /* CRC after data */
#define SECTOR_STREAM_SIZE  (SECTOR_HEADER_SIZE + MFM_SECTOR_SIZE + SECTOR_TRAILER_SIZE)

/* MFM byte period in 68000 cycles.
 * Real hardware: 7,833,600 Hz / 62,500 bytes/sec ≈ 125 cycles.
 * Must be >20 so the ROM can clear RDDATA (via write-bank reg 0 read)
 * and see RDDATA=0 on the STATUS0 read ~20 cycles later.  With
 * MFM_BYTE_CYCLES=1 the ROM's ISM init loop always sees RDDATA=1
 * and never exits. */
#define MFM_BYTE_CYCLES 125

/* Bytes per revolution: 18 sectors * (574 stream + 40 gap) = 11052 bytes.
 * At MFM_BYTE_CYCLES (125) per byte = 1,381,500 CPU cycles per revolution. */
#define BYTES_PER_REVOLUTION  (MFM_SECTORS_PER_TRACK * (SECTOR_STREAM_SIZE + 40))
#define CYCLES_PER_REVOLUTION (BYTES_PER_REVOLUTION * MFM_BYTE_CYCLES)

typedef struct {
    uint8_t lines;          /* Current state of IWM address lines */
    uint8_t mode;           /* IWM mode register */

    uint8_t *disk_data;     /* Pointer to disk image in PSRAM (writable) */
    uint32_t disk_size;
    bool disk_inserted;     /* Reported to ROM via sense lines */
    bool has_disk_image;    /* True if disk data was provided at init */

    /* ISM mode state */
    bool ism_mode;          /* True when SWIM is in ISM mode */
    uint8_t ism_setup;      /* ISM setup register */
    uint8_t ism_mode0;      /* ISM mode0 register */
    uint8_t ism_mode1;      /* ISM mode1 register */
    uint8_t ism_phase;      /* ISM phase register */
    int ism_switch_state;   /* State machine for IWM->ISM mode switch */
    uint8_t ism_last_mode_write; /* Last mode reg value (for ISM switch detection) */

    int track;              /* Current track (0-79) */
    int side;               /* Current side (0-1) */
    int step_direction;     /* 0 = toward 0, 1 = toward 79 */

    /* Sector read state machine — dual pointer model.
     * stream_byte/stream_sector: where the disk clock has gotten to (real-time).
     * read_byte/read_sector: where the CPU's DATA reads have gotten to.
     * RDDATA is set when stream position > read position (unread bytes).
     * This models the SWIM's serial latch: bytes arrive from disk at 500kbps,
     * the CPU reads them sequentially via DATA register. */
    bool read_active;       /* True when we're serving sector data */
    int read_sector;        /* CPU's current sector (DATA read position) */
    int read_byte;          /* CPU's byte position within sector stream */
    int stream_sector;      /* Disk clock's current sector */
    int stream_byte;        /* Disk clock's byte position */
    int total_bytes_read;   /* Total data bytes served (for logging) */

    /* MARK register — two-level model:
     * stream_mark: set when the STREAM reaches a sync byte (A1).
     *   Used by update_byte_clock to stop batch advancement at sync
     *   positions so the CPU can detect them.
     * CPU-visible MARK (returned by ism_read ISM_MARK): computed from
     *   the READ position — reflects whether the byte currently in the
     *   DATA holding register is a sync byte.  This is what the ROM
     *   checks before reading sector data. */
    uint8_t ism_mark;          /* Stream mark — for sync-stop only */

    /* Running CRC register: updated with each byte read from DATA */
    uint16_t ism_crc;
    bool crc_read_low;  /* Toggle: false=return high byte, true=return low byte */

    /* RDDATA handshake: driven by MFM byte arrival clock.
     * Cleared when DATA is read.  Set when enough CPU cycles have
     * elapsed for the next byte to arrive from the disk (~125 cycles
     * at 500 kbps MFM, 7.83 MHz 68000). */
    bool rddata_ready;
    int  rddata_cycle_count;        /* CPU cycles since last DATA read */
    int  prev_cycles_remaining;     /* m68k_cycles_remaining() at last ISM access */

    /* INDEX pulse: signals once per disk revolution (every 18 sectors).
     * Real drives have a physical index hole; the SWIM reports it in
     * HANDSHAKE register.  Without this, the ROM can't detect track
     * boundaries and loops endlessly re-reading the same track. */
    int revolution_count;           /* How many revolutions completed */
    int index_pulse_countdown;      /* Bytes remaining in INDEX pulse window */

    /* Revolution timing: separate from stream position so iwm_tick
     * can generate INDEX pulses without advancing the byte stream.
     * The stream only advances via update_byte_clock() during ISM reads. */
    uint32_t revolution_cycle_counter;  /* Cycles accumulated for revolution timing */

    /* Gap diagnostics: track register access pattern between ID CRC bytes */
    bool gap_tracing;
    int gap_read_count;

    /* Logging */
    int read_log_count;
    int write_log_count;
    int data_log_count;     /* Separate counter for DATA reads */
    int ism_total_reads;    /* Total ISM reads since mode switch */

    /* Per-sector-read diagnostics: counters reset on MODE0 80->F8 */
    int diag_data_reads;    /* Number of DATA register reads */
    int diag_mark_reads;    /* Number of MARK register reads */
    int diag_sta0_reads;    /* Number of STATUS0 register reads */
    int diag_hshk_reads;    /* Number of HANDSHAKE register reads */
    int diag_crc_reads;     /* Number of CRC register reads */
    int diag_mark_ones;     /* Number of times MARK returned 1 */
    int diag_rddata_ones;   /* Number of times STATUS0 had RDDATA=1 */
    int diag_first_data[8]; /* First 8 DATA bytes read */
    uint8_t diag_crc_vals[2]; /* First 2 CRC register values */
    int diag_report_count;  /* How many reports we've made (limit spam) */

    /* Sense line polling diagnostics: track VBL task disk detection */
    int sense_poll_count;   /* Total sense line reads (write-bank CA=1) */
    int sense_poll_ism;     /* Sense polls while in ISM mode */
} iwm_state_t;

static iwm_state_t iwm;

/* CRC-CCITT: init 0xFFFF, polynomial 0x1021 (used in MFM sector headers/data) */
static uint16_t crc_ccitt(const uint8_t *data, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
        }
    }
    return crc;
}

/* Get disk image offset for track/side/sector */
static uint32_t disk_offset(int track, int side, int sector_1based)
{
    /* sector_1based: 1-18 for MFM */
    return ((track * MFM_SIDES + side) * MFM_SECTORS_PER_TRACK +
            (sector_1based - 1)) * MFM_SECTOR_SIZE;
}

/* Return the MFM byte at a given sector position.
 * sector_0based: 0-17, pos: byte offset within the sector stream. */
static uint8_t get_sector_byte(int sector_0based, int pos)
{
    if (!iwm.disk_data) return 0x4E;

    int sector_1based = sector_0based + 1;  /* 1-18 */
    uint8_t byte = 0x4E;  /* Default: gap fill */

    if (pos < 12) {
        byte = 0x00;            /* Sync field */
    } else if (pos < 15) {
        byte = 0xA1;            /* Sync marks */
    } else if (pos == 15) {
        byte = 0xFE;            /* ID address mark */
    } else if (pos == 16) {
        byte = (uint8_t)iwm.track;
    } else if (pos == 17) {
        byte = (uint8_t)iwm.side;
    } else if (pos == 18) {
        byte = (uint8_t)sector_1based;
    } else if (pos == 19) {
        byte = 0x02;            /* Sector size code: 512 bytes */
    } else if (pos == 20 || pos == 21) {
        uint8_t id_buf[8] = {0xA1, 0xA1, 0xA1, 0xFE,
                             (uint8_t)iwm.track, (uint8_t)iwm.side,
                             (uint8_t)sector_1based, 0x02};
        uint16_t id_crc = crc_ccitt(id_buf, 8);
        byte = (pos == 20) ? (uint8_t)(id_crc >> 8) : (uint8_t)(id_crc & 0xFF);
    } else if (pos < 44) {
        byte = 0x4E;            /* Gap 2 */
    } else if (pos < 56) {
        byte = 0x00;            /* Data sync */
    } else if (pos < 59) {
        byte = 0xA1;            /* Data sync marks */
    } else if (pos == 59) {
        byte = 0xFB;            /* Data address mark */
    } else if (pos < 60 + MFM_SECTOR_SIZE) {
        int data_offset = pos - 60;
        uint32_t img_offset = disk_offset(iwm.track, iwm.side, sector_1based);
        if (img_offset + data_offset < iwm.disk_size) {
            byte = iwm.disk_data[img_offset + data_offset];
        } else {
            byte = 0x00;
        }
    } else if (pos == 60 + MFM_SECTOR_SIZE || pos == 60 + MFM_SECTOR_SIZE + 1) {
        uint8_t crc_hdr[4] = {0xA1, 0xA1, 0xA1, 0xFB};
        uint16_t crc = crc_ccitt(crc_hdr, 4);
        uint32_t img_offset = disk_offset(iwm.track, iwm.side, sector_1based);
        for (int i = 0; i < MFM_SECTOR_SIZE; i++) {
            uint8_t d = (img_offset + i < iwm.disk_size) ?
                        iwm.disk_data[img_offset + i] : 0x00;
            crc ^= (uint16_t)d << 8;
            for (int j = 0; j < 8; j++) {
                crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
            }
        }
        byte = (pos == 60 + MFM_SECTOR_SIZE) ?
               (uint8_t)(crc >> 8) : (uint8_t)(crc & 0xFF);
    }

    return byte;
}

/* Return the byte at the current READ position WITHOUT advancing. */
static uint8_t peek_stream_byte(void)
{
    if (!iwm.read_active) return 0xFF;
    return get_sector_byte(iwm.read_sector, iwm.read_byte);
}

/* Advance the stream by one byte position.
 * Called by the byte clock (iwm_tick / update_byte_clock) to simulate
 * the disk spinning autonomously.  Updates MARK, INDEX pulse, and
 * RDDATA.  On real SWIM hardware, bytes arrive at 500 kbps regardless
 * of whether the CPU reads DATA — this function models that. */
/* Advance the STREAM (disk clock) position by one byte.
 * This models the disk spinning — bytes arrive at 500kbps regardless
 * of CPU reads.  The stream position is separate from the DATA read
 * position (read_byte/read_sector).  RDDATA is set when stream is
 * ahead of the read position. */
static void advance_stream_position(void)
{
    if (!iwm.read_active) return;

    /* Tick INDEX pulse countdown */
    if (iwm.index_pulse_countdown > 0)
        iwm.index_pulse_countdown--;

    iwm.stream_byte++;
    if (iwm.stream_byte >= SECTOR_STREAM_SIZE + 40) {
        iwm.stream_sector = (iwm.stream_sector + 1) % MFM_SECTORS_PER_TRACK;
        iwm.stream_byte = 0;
    }

    /* Update MARK for the byte that just arrived in the DATA holding
     * register.  MARK is "sticky high" — once a sync byte is detected,
     * MARK stays at 1 until the CPU reads DATA (which clears it).
     * This ensures the CPU can always see MARK=1 even if the stream
     * advances through multiple bytes between reads (batch advancement).
     * On real SWIM, bytes arrive one at a time and the CPU catches each
     * MARK transition.  In our emulation, the batch model requires the
     * sticky behavior to avoid silently skipping sync bytes. */
    int pos = iwm.stream_byte;
    if ((pos >= 12 && pos <= 14) || (pos >= 56 && pos <= 58)) {
        iwm.ism_mark = 0xFF;
    } else {
        iwm.ism_mark = 0x00;
    }

    /* Hardware CRC: the SWIM accumulates CRC over bytes in ID and data
     * fields only, not gap bytes.  Reset at sync start, accumulate
     * through the field + 2 CRC bytes, then freeze.
     * ID field:   pos 12-21 (3xA1 + FE + 4 ID + 2 CRC)
     * Data field:  pos 56-573 (3xA1 + FB + 512 data + 2 CRC) */
    if (pos == 12 || pos == 56) {
        iwm.ism_crc = 0xFFFF;
    }
    if ((pos >= 12 && pos <= 21) || (pos >= 56 && pos <= 60 + MFM_SECTOR_SIZE + 1)) {
        uint8_t b = get_sector_byte(iwm.stream_sector, pos);
        iwm.ism_crc ^= (uint16_t)b << 8;
        for (int j = 0; j < 8; j++) {
            iwm.ism_crc = (iwm.ism_crc & 0x8000) ?
                ((iwm.ism_crc << 1) ^ 0x1021) : (iwm.ism_crc << 1);
        }
    }

    /* RDDATA: stream is ahead of read position → byte available */
    iwm.rddata_ready = true;
}

/* Advance the DATA READ position by one byte.
 * Called by DATA register reads to deliver the next sequential byte.
 * Updates MARK to reflect the new read position.
 *
 * Overrun model: on real SWIM hardware, if the CPU hasn't read DATA
 * before the next byte arrives, the old byte is overwritten (overrun).
 * The CPU always reads the MOST RECENT byte, not a queued backlog.
 * We model this by snapping the read position to the stream position
 * when the gap exceeds a threshold.  During ISM init (fast reads,
 * gap 0-3 bytes), sequential delivery is preserved.  During the
 * flush routine (slow reads, gap 100+ bytes), overrun fires and
 * the CPU reads near the current disk head position. */
#define OVERRUN_THRESHOLD 200  /* bytes — ISM init gap can reach ~50, flush gap is 30000+ */

static void advance_read_position(void)
{
    if (!iwm.read_active) return;

    /* Check for overrun: snap read to stream if too far behind */
    int stream_abs = iwm.stream_sector * (SECTOR_STREAM_SIZE + 40) + iwm.stream_byte;
    int read_abs = iwm.read_sector * (SECTOR_STREAM_SIZE + 40) + iwm.read_byte;
    int bpr = BYTES_PER_REVOLUTION;
    int gap = (stream_abs - read_abs + bpr) % bpr;

    if (gap > OVERRUN_THRESHOLD) {
        /* Overrun: snap read to one byte before stream position.
         * This simulates the SWIM's serial latch behavior — the
         * CPU gets the most recent byte, all intermediate bytes
         * were lost. */
        int snap_abs = (stream_abs - 1 + bpr) % bpr;
        int sector_stride = SECTOR_STREAM_SIZE + 40;
        iwm.read_sector = snap_abs / sector_stride;
        iwm.read_byte = snap_abs % sector_stride;
    }

    iwm.read_byte++;
    if (iwm.read_byte >= SECTOR_STREAM_SIZE + 40) {
        iwm.read_sector = (iwm.read_sector + 1) % MFM_SECTORS_PER_TRACK;
        iwm.read_byte = 0;
    }

    int pos = iwm.read_byte;

    /* MARK is now computed on-the-fly in ism_read(ISM_MARK) from
     * read_byte, so no MARK update needed here.  The stream_mark
     * (iwm.ism_mark) is only set by advance_stream_position for
     * sync-stop purposes. */

    /* Track data bytes read */
    if (pos >= 60 && pos < 60 + MFM_SECTOR_SIZE) {
        iwm.total_bytes_read++;
    }

    /* RDDATA: cleared after read — next byte arrives in MFM_BYTE_CYCLES */
    iwm.rddata_ready = false;
}

/* Update byte clock using intra-slice cycle tracking.
 * Called at the top of every ISM register access for sub-slice
 * resolution.  Advances the stream position for each elapsed
 * byte period — the disk spins continuously regardless of CPU reads. */
static void update_byte_clock(void)
{
    if (!iwm.read_active) return;
    int remaining = m68k_cycles_remaining();
    if (iwm.prev_cycles_remaining > remaining) {
        int elapsed = iwm.prev_cycles_remaining - remaining;
        iwm.rddata_cycle_count += elapsed;
        iwm.revolution_cycle_counter += elapsed;
    }
    iwm.prev_cycles_remaining = remaining;
    /* Cap rddata_cycle_count to prevent unbounded growth during long
     * non-ISM periods (e.g., display rendering between flush calls).
     * CRITICAL 1: the cap must NOT be a multiple of BYTES_PER_REVOLUTION,
     * or the stream wraps to the exact same position as the read
     * pointer (gap = 0) and the overrun model never fires.
     * CRITICAL 2: the cap must NOT be a multiple of the sector stride
     * (SECTOR_STREAM_SIZE + 40 = 614 bytes), or the stream always
     * advances by the same number of sectors and only visits a subset
     * (e.g., half-revolution = 5526 = 9*614 → only sectors 0 and 9).
     * Adding half a sector stride breaks both resonances. */
    {
        int sector_stride = SECTOR_STREAM_SIZE + 40;
        int max_bytes = BYTES_PER_REVOLUTION / 2 + sector_stride / 2;
        int max_cycles = max_bytes * MFM_BYTE_CYCLES;
        if (iwm.rddata_cycle_count > max_cycles)
            iwm.rddata_cycle_count = max_cycles;
    }

    while (iwm.rddata_cycle_count >= MFM_BYTE_CYCLES) {
        iwm.rddata_cycle_count -= MFM_BYTE_CYCLES;
        advance_stream_position();
        /* When the stream hits a sync byte (MARK=1), stop advancing
         * to give the CPU a chance to see it on the next MARK read.
         * Remaining cycles are saved in rddata_cycle_count for the
         * next update_byte_clock call.  Without this, multiple bytes
         * advance per call and sync positions are silently skipped. */
        if (iwm.ism_mark) break;
    }
    /* INDEX pulse from revolution counter (same logic as iwm_tick) */
    while (iwm.revolution_cycle_counter >= CYCLES_PER_REVOLUTION) {
        iwm.revolution_cycle_counter -= CYCLES_PER_REVOLUTION;
        iwm.revolution_count++;
        iwm.index_pulse_countdown = 30;
        if (iwm.revolution_count <= 5 || (iwm.revolution_count % 50 == 0)) {
            ESP_LOGI(TAG, "REV[%d] trk=%d side=%d total_ism=%d (clock)",
                     iwm.revolution_count, iwm.track, iwm.side,
                     iwm.ism_total_reads);
        }
    }
}

/* ISM mode read handler — only called for read-bank (A12=1, raw_reg >= 8) */
static uint8_t ism_read(int reg)
{
    uint8_t val = 0;

    /* Advance the byte clock on EVERY ISM register read.
     * This keeps prev_cycles_remaining current so that the elapsed
     * time calculated by the next update_byte_clock() only reflects
     * cycles since the most recent ISM access — not since some
     * much-earlier access.  Critical for RDDATA timing: when DATA
     * is read (clearing RDDATA + rddata_cycle_count), the next
     * STATUS0 read must count only cycles since that DATA read.
     * Without this, the stale prev_cycles_remaining causes STATUS0
     * to over-count elapsed cycles, advancing the stream and
     * re-firing RDDATA before the ROM expects it.
     *
     * Safe for ISM init fast reads: between DATA reads only ~30-40
     * cycles elapse (< MFM_BYTE_CYCLES=125), so no stream bytes
     * are advanced.  Safe for flush: MODE0 write already advances
     * the stream, and the extra update here adds negligible cycles. */
    update_byte_clock();

    switch (reg) {
    case ISM_DATA:
        /* Advance the DATA read position by 1 byte and return it.
         * advance_read_position() handles overrun: if the stream
         * (disk clock) is more than OVERRUN_THRESHOLD bytes ahead,
         * the read position snaps to the stream position, modeling
         * the real SWIM's overrun behavior.  For fast reads (ISM
         * init), the gap stays ≤4 bytes and sequential delivery
         * is preserved.  For slow reads (flush routine), the gap
         * is 100+ bytes and the CPU reads near the disk head. */
        advance_read_position();
        val = peek_stream_byte();
        /* CRC is accumulated by the stream clock (advance_stream_position),
         * not here — the SWIM hardware CRC runs on every disk byte. */
        /* Clear RDDATA latch — next byte arrives after MFM_BYTE_CYCLES.
         * This matches real SWIM: reading DATA empties the holding
         * register, RDDATA goes low until the next byte shifts in. */
        iwm.rddata_ready = false;
        iwm.rddata_cycle_count = 0;
        /* MARK is NOT cleared by DATA reads — it reflects stream position
         * via advance_stream_position / advance_read_position.  The ROM's
         * stability check at $43572A reads MARK, DATA, MARK and expects
         * MARK to be stable between reads (same value both times). */
        break;

    case ISM_MARK:
        /* CPU-visible MARK: computed from the READ position.
         * This tells the ROM whether the byte that DATA would return
         * is a sync byte (A1 with missing clock).  The stream-mark
         * (iwm.ism_mark) is separate and only used for sync-stop. */
        {
            int rpos = iwm.read_byte;
            if ((rpos >= 12 && rpos <= 14) || (rpos >= 56 && rpos <= 58))
                val = 0xFF;
            else
                val = 0x00;
        }
        break;

    case ISM_CRC:
        /* Return high byte of running CRC accumulator.
         * After the ID or data CRC bytes have been read through DATA,
         * the accumulator should be 0x0000 (valid field), so this
         * returns 0x00.  During intermediate reads it returns the
         * partial CRC high byte (non-zero). */
        val = (uint8_t)(iwm.ism_crc >> 8);
        break;

    case ISM_PARAM:
        val = 0x00;
        break;

    case ISM_PHASE:
        val = iwm.ism_phase;
        {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            static int phase_log = 0;
            /* Log handshake reads — only the F5/F6/F7 pattern, skip the polling at $4351B2 */
            if (phase_log < 10 && (val == 0xF5 || val == 0xF6 || val == 0xF7) && pc != 0x4351B2) {
                ESP_LOGW(TAG, "HSHK READ: PHASE=%02X PC=%06X mode0=%02X",
                         val, pc, iwm.ism_mode0);
                phase_log++;
            }
            /* Patch $17D (ISM-active flag) when handshake completes.
             * On real Mac, the .SONY Prime entry ($435406) calls $435A88
             * which sets DiskVars+$17D = $FF after verifying the ISM
             * handshake.  But the boot flow goes through SECTOR-VERIFY
             * ($43477E) which skips $435A88.  Without $17D, the sector
             * read at $435196 always takes the IWM/GCR path which can't
             * read MFM disks.  Set $17D here when PHASE reads back F7
             * (final handshake step) to emulate what $435A88 would do. */
            if (val == 0xF7) {
                uint8_t *ram = mac_mem_get_ram();
                if (ram) {
                    uint32_t dv_ptr = ((uint32_t)ram[0x134]<<24)|
                                      ((uint32_t)ram[0x135]<<16)|
                                      ((uint32_t)ram[0x136]<<8)|
                                      ram[0x137];
                    uint32_t ram_sz = mac_mem_get_ram_size();
                    if (dv_ptr > 0 && dv_ptr + 0x19E < ram_sz) {
                        /* Patch $17D (ISM-active flag) */
                        if (ram[dv_ptr + 0x17D] == 0x00) {
                            ram[dv_ptr + 0x17D] = 0xFF;
                            ESP_LOGW(TAG, "PATCH: Set DiskVars+$17D=FF (ISM active) DV=$%06lX",
                                     (unsigned long)dv_ptr);
                        }
                        /* Patch $13E (ISM calibration-done flag).
                         * The ISM probe at $435CF2 checks TST.B $13E(A1):
                         *   $13E=0 → falls through to CMPI.W #$CE,D1 gate
                         *     which ONLY passes for drive index 3 (internal
                         *     floppy on original hardware).  Our drive maps
                         *     to index 5 (D1=$019A) and always fails.
                         *   $13E≠0 → fast path: writes MODE0/MODE1 config
                         *     and returns without checking D1.
                         * Setting $13E=FF tells the ROM the ISM has already
                         * been calibrated, bypassing the D1==$CE restriction
                         * and allowing ISM sector reads for all HD drives. */
                        if (ram[dv_ptr + 0x13E] == 0x00) {
                            ram[dv_ptr + 0x13E] = 0xFF;
                            ESP_LOGW(TAG, "PATCH: Set DiskVars+$13E=FF (ISM calibrated)");
                        }
                        /* Populate drive 5 entry (internal floppy).
                         * SEC-CHK at $4357D8 does TST.B (+$04,A1,D1.W);
                         * BMI = fail.  So byte +$04 must be non-negative. */
                        {
                            uint8_t d5_04 = ram[dv_ptr + 0x019A + 0x04];
                            if (d5_04 & 0x80) {
                                ram[dv_ptr + 0x019A + 0x03] = 0x00;
                                ram[dv_ptr + 0x019A + 0x04] = 0x01;
                                ESP_LOGW(TAG, "PATCH: Drive 5 +$04: %02X->01", d5_04);
                            }
                        }
                    }
                }
            }
        }
        break;

    case ISM_SETUP:
        val = iwm.ism_setup;
        break;

    case ISM_MODE0:
        /* STATUS0 (read) is different from MODE0 (write).
         * ISM-mode STATUS0 bit layout (derived from ROM comparison
         * at 43548C which expects STATUS0 & 0x1F == 0x17):
         *   Bit 7: RDDATA  (byte available in DATA reg)
         *   Bit 6: WRDATA  (write reg empty, 0 during reads)
         *   Bit 5: OVRN    (overrun error — always 0 in emulation)
         *   Bit 4: MOTORON
         *   Bit 3: CRCERR  (CRC error — always 0)
         *   Bit 2: ENABLE  (read/write active)
         *   Bit 1: MFM     (1 = MFM mode)
         *   Bit 0: PRESENT (disk in drive)  */
        val = 0;
        if (iwm.read_active && iwm.rddata_ready)
            val |= 0x80;   /* Bit 7: RDDATA */
        if (iwm.disk_inserted)
            val |= 0x10;   /* Bit 4: MOTORON */
        if (iwm.read_active)
            val |= 0x04;   /* Bit 2: ENABLE (drive enabled for read) */
        if (iwm.ism_setup & ISM_SETUP_GCRMFM)
            val |= 0x02;   /* Bit 1: MFM mode */
        if (iwm.disk_inserted)
            val |= 0x01;   /* Bit 0: FD_PRESENT */
        /* Debug: log when read_active but no rddata */
        if (iwm.read_active && !iwm.rddata_ready) {
            static int sta0_dbg = 0;
            if (sta0_dbg++ < 5) {
                uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
                ESP_LOGW(TAG, "STA0: active but rdy=0 cyc=%d strm=%d/%d read=%d/%d mark=%d PC=%06X",
                         iwm.rddata_cycle_count, iwm.stream_sector, iwm.stream_byte,
                         iwm.read_sector, iwm.read_byte, iwm.ism_mark, pc);
            }
        }
        break;

    case ISM_HANDSHAKE:
        val = 0;
        if (iwm.read_active && iwm.disk_inserted && iwm.rddata_ready) {
            val |= ISM_HSHK_RDDATA;
        }
        if (iwm.disk_inserted) {
            val |= ISM_HSHK_MOTORON;
            /* SENSE/FDHD (bit 5): on SuperDrive, LOW = HD media detected,
             * HIGH = DD media or no disk.  Our 1.44MB image is HD, so
             * SENSE should be 0 when a disk is inserted.  The ROM checks
             * this bit to decide ISM (HD/MFM) vs IWM (DD/GCR) path. */
            /* ISM_HSHK_SENSE intentionally NOT set for HD disk */
        }
        /* ERROR bit (bit 3): reflects CRC status of last completed field.
         * On real SWIM, when no read is active the CRC result is undefined
         * (no valid field was processed), so ERROR is set.
         * The ROM's init path at $43580C reads HANDSHAKE via $435198/$435C92
         * and expects ERROR=1 (BTST #3) so BMI $43583C is taken to advance
         * the drive state machine.  Without this, the init path exits early
         * at $435832 (BRA $435856) and the drive state never increments.
         *
         * IMPORTANT: After mac_boot_read_bypass is active (F600+), the
         * DM-READ intercept at $40315C handles all .SONY reads.  We must
         * NOT report ERROR here anymore — the .SONY VBL task reads
         * HANDSHAKE and ERROR=1 triggers the ISM init path ($435824)
         * which corrupts the PC (crashes to $00014D in RAM data).
         * With ERROR=0, the VBL task takes the "everything OK" path
         * at $435832 and exits cleanly. */
        if (!iwm.read_active && iwm.ism_mode && iwm.disk_inserted
            && !mac_boot_read_bypass) {
            val |= ISM_HSHK_ERROR;
        }
        /* INDEX pulse: bit 2 (0x04) — active briefly at revolution boundary */
        if (iwm.index_pulse_countdown > 0) {
            val |= 0x04;
        }
        /* Log HANDSHAKE reads when ISM not actively reading.
         * This captures the init path's BSR $435198 check which
         * happens inside a 1000-cycle slice (invisible to PC trace). */
        if (!iwm.read_active && iwm.ism_mode && iwm.disk_inserted) {
            static int hshk_inactive_log = 0;
            if (hshk_inactive_log < 3) {
                uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
                ESP_LOGW(TAG, "HSHK-INACTIVE: val=%02X PC=%06X err=%d",
                         val, pc, (val & ISM_HSHK_ERROR) ? 1 : 0);
                hshk_inactive_log++;
            }
        }
        break;
    }

    /* ISM read tracing */
    iwm.ism_total_reads++;

    /* Per-sector-read diagnostics */
    switch (reg) {
    case ISM_DATA:
        if (iwm.diag_data_reads < 8) {
            iwm.diag_first_data[iwm.diag_data_reads] = val;
        }
        iwm.diag_data_reads++;
        break;
    case ISM_MARK:
        iwm.diag_mark_reads++;
        if (val) iwm.diag_mark_ones++;
        break;
    case ISM_CRC:
        if (iwm.diag_crc_reads < 2) {
            iwm.diag_crc_vals[iwm.diag_crc_reads] = val;
        }
        iwm.diag_crc_reads++;
        break;
    case ISM_MODE0:
        iwm.diag_sta0_reads++;
        if (val & 0x80) iwm.diag_rddata_ones++;
        break;
    case ISM_HANDSHAKE:
        iwm.diag_hshk_reads++;
        break;
    default: break;
    }

    /* Detailed trace for ISM reads during active F8 windows (first session only) */
    if (iwm.read_active && iwm.diag_report_count < 1) {
        int total_diag = iwm.diag_sta0_reads + iwm.diag_mark_reads +
                         iwm.diag_data_reads + iwm.diag_hshk_reads + iwm.diag_crc_reads;
        if (total_diag <= 10) {
            static const char *rnames[] = {"DATA","MARK","CRC ","PARM","PHAS","SETU","STA0","HSHK"};
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            ESP_LOGW(TAG, "ISM-TR %s=%02X rpos=%d/%d spos=%d/%d rdy=%d cyc=%d PC=%06X",
                     rnames[reg], val,
                     iwm.read_byte, iwm.read_sector,
                     iwm.stream_byte, iwm.stream_sector,
                     iwm.rddata_ready ? 1 : 0,
                     iwm.rddata_cycle_count, pc);
        }
    }

    if (reg == ISM_DATA) {
        int pos = iwm.read_byte - 1;
        /* Log sector data starts */
        if (pos == 60) {
            ESP_LOGI(TAG, "SEC[%d] trk=%d side=%d first=%02X rev=%d total=%d",
                     iwm.read_sector + 1, iwm.track, iwm.side,
                     val, iwm.revolution_count, iwm.total_bytes_read);
        }
        /* Log ID mark detection (FE byte at pos=15) */
        if (pos == 15 && val == 0xFE && iwm.revolution_count < 3) {
            ESP_LOGI(TAG, "ID: trk=%d side=%d sec=%d rev=%d",
                     iwm.track, iwm.side, iwm.read_sector + 1,
                     iwm.revolution_count);
        }
    }

    return val;
}

/* ISM mode write handler */
static void ism_write(int reg, uint8_t value)
{
    switch (reg) {
    case ISM_DATA:
        /* Write data register (for disk writes - not implemented) */
        break;

    case ISM_MARK:
        /* Write mark register */
        break;

    case ISM_CRC:
        /* Writing to CRC register: clears the CRC error flag.
         * Does NOT reset the running accumulator — that only resets
         * when the ISM detects an A1 address mark with missing clock. */
        break;

    case ISM_PARAM:
        /* Parameter data */
        break;

    case ISM_PHASE: {
        uint32_t wr_pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
        iwm.ism_phase = value;
        {
            static int pw_log = 0;
            if (pw_log < 5 && (value == 0xF5 || value == 0xF6 || value == 0xF7)) {
                ESP_LOGW(TAG, "HSHK WRITE: PHASE<-%02X PC=%06X mode0=%02X",
                         value, wr_pc, iwm.ism_mode0);
                pw_log++;
            }
        }
        /* Log ISM init starts (PHAS=$F5 write) — limited to reduce spam. */
        if (value == 0xF5 && !iwm.read_active) {
            static int init_log = 0;
            if (init_log < 5) {
                uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
                ESP_LOGW(TAG, "ISM-INIT-START PC=%06X", pc);
                init_log++;
            }
        }
        /* Log PHASE=$F2 writes — the init path at $43581E writes PHASE=$F2
         * before the HANDSHAKE check.  This confirms the init path was reached. */
        if (value == 0xF2) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
            static int f2_log = 0;
            if (f2_log < 10) {
                ESP_LOGW(TAG, "*** PHASE-F2 *** PC=%06X mode0=%02X active=%d (init path!)",
                         pc, iwm.ism_mode0, iwm.read_active);
                f2_log++;
            }
        }
        break;
    }

    case ISM_SETUP:
        iwm.ism_setup = value;
        break;

    case ISM_MODE0:
        {
            uint8_t old = iwm.ism_mode0;
            iwm.ism_mode0 = value;

            /* Advance the byte clock on MODE0 writes.  The flush
             * routine writes MODE0=80 before each DATA read.  By
             * advancing the stream here, the subsequent DATA read's
             * advance_read_position() sees a large gap and snaps
             * the read position forward (overrun model).  During
             * ISM init, MODE0 is only written once (F8 activation),
             * so this doesn't affect the init's sequential reads. */
            if (iwm.read_active) {
                update_byte_clock();
            }

            /* Head select: bit 0 of MODE0 controls which side
             * (0=side 0/lower, 1=side 1/upper) */
            iwm.side = (value & 0x01) ? 1 : 0;

            /* Log MODE0 only on significant transitions:
             * enable/disable (0↔non-0), side change, or read_active toggle */
            if ((value == 0) != (old == 0) ||
                (value & 0x01) != (old & 0x01) ||
                value != old) {
                uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
                ESP_LOGI(TAG, "MODE0: %02X->%02X side=%d active=%d sec=%d/%d rev=%d PC=%06X",
                         old, value, iwm.side, iwm.read_active,
                         iwm.read_sector, iwm.read_byte,
                         iwm.revolution_count, pc);

                /* Dump sector-read diagnostics on F8→80 transition */
                if (old == 0xF8 && value == 0x80 && iwm.diag_report_count < 10) {
                    ESP_LOGW(TAG, "DIAG: STA0=%d(rd=%d) MARK=%d(=1:%d) DATA=%d CRC=%d[%02X,%02X] HSHK=%d strm=%d/%d",
                             iwm.diag_sta0_reads, iwm.diag_rddata_ones,
                             iwm.diag_mark_reads, iwm.diag_mark_ones,
                             iwm.diag_data_reads, iwm.diag_crc_reads,
                             iwm.diag_crc_vals[0], iwm.diag_crc_vals[1],
                             iwm.diag_hshk_reads,
                             iwm.stream_sector, iwm.stream_byte);
                    if (iwm.diag_data_reads > 0) {
                        int n = iwm.diag_data_reads < 8 ? iwm.diag_data_reads : 8;
                        char buf[64];
                        int pos = 0;
                        for (int j = 0; j < n; j++)
                            pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X ", iwm.diag_first_data[j]);
                        ESP_LOGW(TAG, "DIAG first %d DATA: %s", n, buf);
                    }
                    iwm.diag_report_count++;
                }

                /* Compact DiskVars dump on F8→80 transition (first 3 only). */
                if (old == 0xF8 && value == 0x80 && iwm.diag_report_count <= 3) {
                    uint8_t *ram = mac_mem_get_ram();
                    if (ram) {
                        uint32_t dv_ptr = ((uint32_t)ram[0x134]<<24)|((uint32_t)ram[0x135]<<16)|
                                          ((uint32_t)ram[0x136]<<8)|ram[0x137];
                        if (dv_ptr > 0 && dv_ptr < 0x200000) {
                            uint8_t *dv = ram + dv_ptr;
                            ESP_LOGW(TAG, "DV: $12=%02X%02X $13E=%02X $17D=%02X +3=%02X +5=%02X",
                                     dv[0x12], dv[0x13], dv[0x13E], dv[0x17D],
                                     dv[0x019A + 0x03], dv[0x019A + 0x05]);
                        }
                    }
                }
            }

            /* Reset per-sector-read diagnostic counters on MODE0 change */
            if (value != old) {
                iwm.diag_data_reads = 0;
                iwm.diag_mark_reads = 0;
                iwm.diag_sta0_reads = 0;
                iwm.diag_hshk_reads = 0;
                iwm.diag_crc_reads = 0;
                iwm.diag_mark_ones = 0;
                iwm.diag_rddata_ones = 0;
                memset(iwm.diag_first_data, 0, sizeof(iwm.diag_first_data));
                memset(iwm.diag_crc_vals, 0, sizeof(iwm.diag_crc_vals));
            }

            /* Drive enable: bits 3-6 of MODE0 (0x78) control read activation.
             * Bit 3 = LSTRB (read strobe), bits 4-6 = MOTORON/ACTION/etc.
             * HDSEL (bit 7) and CA lines (bits 0-2) don't enable reads.
             * Real SWIM: MODE0=F8 activates, MODE0=80/04/02 deactivates. */
            if (value & 0x78) {
                if (!iwm.read_active && iwm.disk_inserted) {
                    /* Real SWIM: the disk spins and the shift register
                     * clocks continuously, even when read mode is off.
                     * When read mode is enabled, the most recent byte
                     * is already in the holding register (RDDATA=1).
                     * Start at byte 8 (gap, before sync at 12) so
                     * MARK=0 on first read.  The stream will reach
                     * sync after a few byte periods. */
                    iwm.rddata_ready = true;
                    iwm.rddata_cycle_count = MFM_BYTE_CYCLES;
                    iwm.revolution_cycle_counter = 0;
                    iwm.prev_cycles_remaining = m68k_cycles_remaining();
                    iwm.read_active = true;
                    iwm.read_sector = 0;
                    iwm.read_byte = 8;
                    iwm.stream_sector = 0;
                    iwm.stream_byte = 8;
                    iwm.total_bytes_read = 0;
                    iwm.ism_crc = 0xFFFF;
                    iwm.data_log_count = 0;
                    uint32_t off = disk_offset(iwm.track, iwm.side, 1);
                    ESP_LOGW(TAG, "ISM read START: trk=%d side=%d off=%lu",
                             iwm.track, iwm.side, (unsigned long)off);
                }
            } else {
                if (iwm.read_active) {
                    ESP_LOGI(TAG, "ISM read STOP: trk=%d side=%d rev=%d sec=%d/%d",
                             iwm.track, iwm.side, iwm.revolution_count,
                             iwm.read_sector, iwm.read_byte);
                    iwm.read_active = false;
                }
            }
        }
        break;

    case ISM_HANDSHAKE:
        iwm.ism_mode1 = value;
        break;
    }
}

void iwm_init(uint8_t *disk_data, uint32_t disk_size)
{
    memset(&iwm, 0, sizeof(iwm));
    iwm.disk_data = disk_data;
    iwm.disk_size = disk_size;
    iwm.has_disk_image = (disk_data != NULL && disk_size > 0);
    /* Start with disk NOT inserted.  The emulator main loop will
     * call iwm_insert_disk() after the ROM's event loop has started
     * so the ROM sees the "no disk → disk present" transition and
     * posts the disk insertion event to the event queue. */
    iwm.disk_inserted = false;

    if (iwm.has_disk_image) {
        ESP_LOGI(TAG, "IWM init: %lu byte disk image ready (deferred insert)",
                 (unsigned long)disk_size);
        if (disk_size >= 4) {
            ESP_LOGI(TAG, "  Boot block: %02X %02X %02X %02X",
                     disk_data[0], disk_data[1], disk_data[2], disk_data[3]);
        }
    } else {
        ESP_LOGI(TAG, "IWM initialized (no disk)");
    }
}

void iwm_insert_disk(void)
{
    if (iwm.has_disk_image && !iwm.disk_inserted) {
        iwm.disk_inserted = true;
        /* Reset log counters so we can see the ROM's reaction */
        iwm.read_log_count = 0;
        iwm.write_log_count = 0;
        ESP_LOGW(TAG, "*** DISK INSERTED *** (%lu bytes) lines=%02X mode=%02X ism=%d",
                 (unsigned long)iwm.disk_size, iwm.lines, iwm.mode, iwm.ism_mode);

        /* Populate drive 5 entry in DiskVars so the .SONY driver
         * recognizes the floppy drive.  On a real Mac, the driver's
         * interrupt handler populates this when a disk is detected.
         * Our boot flow bypasses that, so we do it manually.
         *
         * Drive 5 entry starts at DiskVars + $019A (D1 from $435122).
         * The SEC-CHK at $4357D8 does TST.B ($04,A1,D1.W) — byte at
         * offset +$04 must be non-negative (bit 7 = 0) for the drive
         * to be accepted.
         * Byte +$03 should be 0 (idle) so the BEQ path at $4357E4
         * is taken (drives 4+ with +$03 > 0 hit a drive# >= 4 check). */
        uint8_t *ram = mac_mem_get_ram();
        if (ram) {
            uint32_t dv_ptr = ((uint32_t)ram[0x134]<<24)|
                              ((uint32_t)ram[0x135]<<16)|
                              ((uint32_t)ram[0x136]<<8)|
                              ram[0x137];
            uint32_t ram_sz = mac_mem_get_ram_size();
            if (dv_ptr > 0 && dv_ptr + 0x019E < ram_sz) {
                /* Only patch if byte is negative (uninitialized/0xFF) */
                uint8_t cur = ram[dv_ptr + 0x019A + 0x04];
                if (cur & 0x80) {
                    ram[dv_ptr + 0x019A + 0x03] = 0x00;  /* idle */
                    ram[dv_ptr + 0x019A + 0x04] = 0x01;  /* installed (SuperDrive) */
                    ESP_LOGW(TAG, "PATCH: Drive 5 entry DV+$019E: %02X->01 (installed)",
                             cur);
                }
            }
        }
    }
}

uint8_t iwm_read(uint32_t address)
{
    int reg = (address >> 9) & 0xF;

    /* ISM mode: A2-A0 on SWIM (= bits 2-0 of IWM reg) select register.
     * R/W comes from the 68000 bus cycle, not address lines. */
    if (iwm.ism_mode) {
        int ism_reg = reg & 0x07;
        /* In ISM mode, A12 distinguishes register banks:
         * A12=0 (raw reg 0-7):  IWM-compatible drive control reads.
         *   These toggle drive state lines (CA0, CA1, LSTRB, etc.)
         *   but do NOT read ISM registers or consume data stream bytes.
         * A12=1 (raw reg 8-15): Actual ISM register reads (DATA, MARK,
         *   CRC, STATUS0, HANDSHAKE, etc.)
         * The ROM uses write-bank reads (tst.b offset(a0)) for drive
         * control and read-bank reads for data/status. */
        if (reg < 8) {
            /* Write-bank (A12=0): IWM-compatible drive control reads.
             * These toggle IWM state lines (CA0/CA1/CA2/LSTRB) and
             * return IWM drive sense values — NOT ISM register values.
             * The ROM uses TST.B on these return values to check drive
             * status (disk in place, write protect, track 0, etc.) */
            int line_bit = 1 << (reg >> 1);
            if (reg & 1) {
                iwm.lines |= line_bit;
            } else {
                iwm.lines &= ~line_bit;
            }
            /* Track head stepping via phase line toggles.
             * Same logic as IWM mode: when CA0 + LSTRB are both set,
             * CA1 determines step direction. */
            if (reg <= 3) {
                if ((iwm.lines & IWM_CA0) && (iwm.lines & IWM_LSTRB)) {
                    int old_track = iwm.track;
                    if (iwm.lines & IWM_CA1) {
                        if (iwm.track < MFM_TRACKS - 1) iwm.track++;
                    } else {
                        if (iwm.track > 0) iwm.track--;
                    }
                    if (iwm.track != old_track) {
                        static int step_log = 0;
                        if (step_log < 20) {
                            ESP_LOGI(TAG, "ISM step: trk %d->%d", old_track, iwm.track);
                            step_log++;
                        }
                    }
                }
            }
            /* Side select from SEL line */
            iwm.side = (iwm.lines & IWM_SELECT) ? 1 : 0;
            /* Write-bank register 0 (ism_reg==0, DATA) clears the RDDATA
             * latch on real SWIM hardware without consuming a byte.
             * This is critical: the ROM's post-sector-read flow at 435154
             * reads write-bank reg 0 (TST.B (A0)) to reset RDDATA, then
             * checks STATUS0 ~100 cycles later expecting RDDATA=0. */
            int ism_reg_wb = reg & 0x07;
            if (ism_reg_wb == ISM_DATA && iwm.read_active) {
                /* Clear RDDATA latch without consuming a byte.
                 * Flush the byte clock first so prev_cycles_remaining
                 * is current — otherwise the next update_byte_clock()
                 * adds stale cycles and re-fires RDDATA immediately.
                 * Also reset rddata_cycle_count so the next byte doesn't
                 * arrive until a full MFM_BYTE_CYCLES (125) have elapsed.
                 * Without this, the fractional remainder from the flush
                 * plus ~40 cycles to the next STATUS0 read can immediately
                 * re-fire RDDATA, and the ROM sees the clear "failed". */
                update_byte_clock();
                iwm.rddata_ready = false;
                iwm.rddata_cycle_count = 0;
            }
            /* Return sense value based on current CA0-CA2 state.
             * Bit 7: 0 = asserted (yes), 0x80 = negated (no).
             * Must respect disk_inserted so ROM sees the transition
             * from "no disk" to "disk present" and posts the disk
             * insertion event to the event queue. */
            int ca = iwm.lines & 0x07;
            uint8_t val;
            switch (ca) {
            case 0: val = 0x00; break;  /* /STEP direction */
            case 1: val = iwm.disk_inserted ? 0x00 : 0x80; break;  /* Disk in place */
            case 2: val = (iwm.track == 0) ? 0x00 : 0x80; break;  /* /TK0 */
            case 3: val = iwm.disk_inserted ? 0xFF : 0x00; break;  /* /WRPROT (1=yes) */
            case 4: val = iwm.read_active ? 0x00 : 0x80; break;  /* Motor */
            case 5: val = (iwm.track == 0) ? 0x80 : 0x00; break;  /* Track 0 */
            case 6: val = 0x00; break;  /* TACH */
            case 7: val = iwm.disk_inserted ? 0x00 : 0x80; break;  /* Drive ready */
            default: val = 0x00; break;
            }
            /* Track sense line polling (disk-in-place checks) */
            if (ca == 1) {
                iwm.sense_poll_count++;
                iwm.sense_poll_ism++;
            }
            /* Trace write-bank reads during ISM init */
            if (iwm.ism_total_reads <= 5 && iwm.read_active) {
                uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
                ESP_LOGI(TAG, "ISM WB[%d] reg=%d ca=%d val=%02X rdclr=%d PC=%06X",
                         iwm.ism_total_reads, reg, ca, val,
                         (ism_reg_wb == ISM_DATA) ? 1 : 0, pc);
            }
            return val;
        }
        return ism_read(ism_reg);
    }

    /* IWM mode: update line state based on address */
    int line_bit = 1 << (reg >> 1);
    if (reg & 1) {
        iwm.lines |= line_bit;
    } else {
        iwm.lines &= ~line_bit;
    }

    /* Update side from current lines (reads also change state) */
    iwm.side = (iwm.lines & IWM_SELECT) ? 1 : 0;

    /* Log IWM reads (minimal) */
    if (iwm.read_log_count < 20) {
        iwm.read_log_count++;
    }

    /* Q6=0, Q7=0: Read data register / sense lines */
    if (!(iwm.lines & IWM_Q6) && !(iwm.lines & IWM_Q7)) {
        if (!iwm.disk_inserted) {
            return 0xFF;    /* No disk = all 1s */
        }

        /* If reading sector data */
        if ((iwm.lines & IWM_ENABLE) && iwm.read_active) {
            return peek_stream_byte();
        }

        /* Sense line values based on CA0-CA2.
         * Must respect disk_inserted for state transition detection. */
        int ca = iwm.lines & 0x07;
        if (ca == 1) iwm.sense_poll_count++;
        uint8_t val;
        switch (ca) {
        case 0: val = 0x00; break;  /* /STEP direction */
        case 1: val = iwm.disk_inserted ? 0x00 : 0xFF; break;  /* Disk in place */
        case 2: val = (iwm.track > 0) ? 0x00 : 0xFF; break;  /* Not at trk0 */
        case 3: val = iwm.disk_inserted ? 0xFF : 0x00; break;  /* /WRPROT */
        case 4: val = (iwm.lines & IWM_ENABLE) ? 0x00 : 0xFF; break;  /* Motor */
        case 5: val = (iwm.track == 0) ? 0x00 : 0xFF; break;  /* Track 0 */
        case 6: val = 0x00; break;  /* TACH */
        case 7: val = iwm.disk_inserted ? 0x00 : 0xFF; break;  /* Disk ready */
        default: val = 0x00; break;
        }

        return val;
    }

    /* Q6=1, Q7=0: Read status register */
    if ((iwm.lines & IWM_Q6) && !(iwm.lines & IWM_Q7)) {
        uint8_t status = iwm.mode & 0x1F;
        if (iwm.lines & IWM_ENABLE) {
            status |= 0x20;
        }
        return status;
    }

    /* Q6=0, Q7=1: Read handshake register */
    if (!(iwm.lines & IWM_Q6) && (iwm.lines & IWM_Q7)) {
        uint8_t hshk;
        if (iwm.disk_inserted && iwm.read_active) {
            hshk = 0xC0;    /* Data ready, no underrun */
        } else {
            hshk = 0x3F;
        }
        return hshk;
    }

    return 0xFF;
}

void iwm_write(uint32_t address, uint8_t value)
{
    int reg = (address >> 9) & 0xF;

    /* ISM mode: ALL writes go to ISM registers (A2-A0 select register).
     * Unlike reads, there is no A12 bank split for writes — the SWIM
     * always routes writes to ISM register logic.  Drive control
     * (stepping, motor, side) is done via address READS in ISM mode,
     * not writes. */
    if (iwm.ism_mode) {
        int ism_reg = reg & 0x07;
        if (iwm.ism_total_reads <= 5) {
            const char *wnames[] = {"DATA","MARK","CRC","PARM","PHAS","SETU","MOD0","MOD1"};
            ESP_LOGI(TAG, "ISM W %s=%02X", wnames[ism_reg], value);
        }
        ism_write(ism_reg, value);
        return;
    }

    /* IWM mode: update line state */
    int line_bit = 1 << (reg >> 1);
    if (reg & 1) {
        iwm.lines |= line_bit;
    } else {
        iwm.lines &= ~line_bit;
    }


    /* Track head stepping via phase lines */
    if (reg <= 3) {
        if ((iwm.lines & IWM_CA0) && (iwm.lines & IWM_LSTRB)) {
            if (iwm.lines & IWM_CA1) {
                if (iwm.track < MFM_TRACKS - 1) iwm.track++;
            } else {
                if (iwm.track > 0) iwm.track--;
            }
        }
    }

    /* Side select: on the Mac Classic, the SEL line controls head select.
     * SEL=0 may mean side 0 (bottom) and SEL=1 side 1, or vice versa.
     * Also update side from CA2 which some drives use. */
    iwm.side = (iwm.lines & IWM_SELECT) ? 1 : 0;

    /* ENABLE line: start/stop reading */
    if (iwm.lines & IWM_ENABLE) {
        if (!iwm.read_active && iwm.disk_inserted) {
            iwm.read_active = true;
            iwm.read_sector = 0;
            iwm.read_byte = 0;
            iwm.stream_sector = 0;
            iwm.stream_byte = 0;
            iwm.total_bytes_read = 0;
            ESP_LOGI(TAG, "IWM read: trk=%d side=%d", iwm.track, iwm.side);
        }
    } else {
        if (iwm.read_active) {
            iwm.read_active = false;
        }
    }

    /* Q6=1, Q7=1: Write mode register */
    if ((iwm.lines & IWM_Q6) && (iwm.lines & IWM_Q7)) {
        iwm.mode = value;

        /* SWIM ISM mode switch detection:
         * The ROM writes an alternating 0x17/0x57 pattern (toggling bit 6)
         * four times, then a final config value (0xF5) to activate ISM.
         * Observed sequence: 0x17, 0x57, 0x17, 0x57, 0xF5 */
        if (iwm.ism_switch_state == 0) {
            if (value == 0x17 || value == 0x57) {
                iwm.ism_switch_state = 1;
                iwm.ism_last_mode_write = value;
            }
        } else if (iwm.ism_switch_state < 4) {
            if (value == (iwm.ism_last_mode_write ^ 0x40)) {
                iwm.ism_switch_state++;
                iwm.ism_last_mode_write = value;
            } else if (value == 0x17 || value == 0x57) {
                /* Restart sequence */
                iwm.ism_switch_state = 1;
                iwm.ism_last_mode_write = value;
            } else {
                iwm.ism_switch_state = 0;
            }
        } else {
            /* 4 alternating writes complete, this write activates ISM */
            iwm.ism_mode = true;
            iwm.ism_switch_state = 0;
            /* Default ISM setup for HD 1.44MB: MFM mode, fast clock */
            iwm.ism_setup = ISM_SETUP_GCRMFM | ISM_SETUP_CLKSPD;
            /* Reset log counters to capture full ISM initialization */
            iwm.read_log_count = 0;
            iwm.write_log_count = 0;
            iwm.ism_total_reads = 0;
            ESP_LOGW(TAG, "*** SWIM -> ISM mode *** (trigger=%02X, setup=%02X)",
                     value, iwm.ism_setup);
        }
    }
}

uint8_t *iwm_get_disk_data(void)
{
    return iwm.disk_data;
}

uint32_t iwm_get_disk_size(void)
{
    return iwm.disk_size;
}

bool iwm_disk_inserted(void)
{
    return iwm.disk_inserted;
}

bool iwm_is_reading(void)
{
    return iwm.ism_mode && iwm.read_active;
}

bool iwm_is_ism_mode(void)
{
    return iwm.ism_mode;
}

int iwm_get_sense_poll_count(void)
{
    return iwm.sense_poll_count;
}

int iwm_get_sense_poll_ism(void)
{
    return iwm.sense_poll_ism;
}

void iwm_tick(int cycles)
{
    if (!iwm.read_active) return;
    /* Between slices: flush remaining cycles from prev_cycles_remaining
     * into the accumulators.  Do NOT advance stream position here —
     * the stream only advances via update_byte_clock() during ISM
     * register reads.  This prevents the tight ROM init loop from
     * missing bytes that fly past between slices (~7 bytes per 1000-cycle
     * slice = skips right over A1 address marks). */
    if (iwm.prev_cycles_remaining > 0) {
        iwm.rddata_cycle_count += iwm.prev_cycles_remaining;
        iwm.revolution_cycle_counter += iwm.prev_cycles_remaining;
    }
    iwm.prev_cycles_remaining = 0;

    /* Revolution timing: generate INDEX pulse based on elapsed cycles,
     * independent of stream byte position. */
    while (iwm.revolution_cycle_counter >= CYCLES_PER_REVOLUTION) {
        iwm.revolution_cycle_counter -= CYCLES_PER_REVOLUTION;
        iwm.revolution_count++;
        iwm.index_pulse_countdown = 30;
        if (iwm.revolution_count <= 5 || (iwm.revolution_count % 50 == 0)) {
            ESP_LOGI(TAG, "REV[%d] trk=%d side=%d total_ism=%d (tick)",
                     iwm.revolution_count, iwm.track, iwm.side,
                     iwm.ism_total_reads);
        }
    }
}

void iwm_start_slice(int cycles)
{
    /* Set prev_cycles_remaining to the new slice size so that
     * update_byte_clock() correctly counts cycles from the start
     * of the slice to the first ISM register access.  Without this,
     * those cycles would be lost (prev=0 after iwm_tick). */
    iwm.prev_cycles_remaining = cycles;
}
