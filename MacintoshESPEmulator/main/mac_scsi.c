/*
 * NCR 5380 SCSI Controller Emulation for Macintosh Classic
 *
 * Minimal emulation: no devices on the SCSI bus.
 * - Arbitration: AIP goes high when MR.ARBITRATE set, then clears
 *   after a few ICR reads (arbitration won, no contention).
 * - Selection always fails (no target asserts BSY).
 * - Phase match tracks TCR vs CSR for correct BSR reporting.
 *
 * This allows apps like HD SC Setup to scan the SCSI bus and
 * report "no devices found" instead of hanging.
 */
#include "mac_scsi.h"
#include <string.h>
#include <esp_log.h>

static const char *TAG = "scsi";

typedef struct {
    uint8_t odr;        /* Output Data Register (written by initiator) */
    uint8_t icr;        /* Initiator Command Register (written bits) */
    uint8_t mr;         /* Mode Register */
    uint8_t tcr;        /* Target Command Register */
    uint8_t ser;        /* Select Enable Register */

    /* Arbitration state machine */
    int     arb_state;  /* 0=idle, 1=in progress (AIP=1), 2=won (AIP=0) */
    int     arb_reads;  /* Number of ICR reads since arbitration started */

    /* Boot mode: all reads return $00 to avoid heap changes from
     * SCSI Manager init.  Set to false after boot completes. */
    bool    boot_mode;

} scsi_state_t;

static scsi_state_t scsi;

void scsi_init(void)
{
    memset(&scsi, 0, sizeof(scsi));
    scsi.boot_mode = true;  /* Start in boot mode: flat $00 returns */
    ESP_LOGI(TAG, "NCR 5380 SCSI initialized (boot mode, no devices)");
}

void scsi_set_active(bool active)
{
    if (active && scsi.boot_mode) {
        scsi.boot_mode = false;
        ESP_LOGI(TAG, "NCR 5380 SCSI: full register emulation enabled");
    }
}

/*
 * Register decode: Mac Classic uses address bits A6-A4.
 * register = (address >> 4) & 0x07
 * Confirmed by ROM access patterns: $5FFxx0/1 where xx = reg*1.
 * $5FF001=reg0, $5FF010/11=reg1, $5FF021=reg2, etc.
 */
static inline int scsi_reg(uint32_t address)
{
    return (address >> 4) & 0x07;
}

uint8_t scsi_read(uint32_t address)
{
    int reg = scsi_reg(address);
    uint8_t val = 0;
    /* Boot mode: return $00 for all registers (matches pre-SCSI behavior).
     * This prevents the ROM's SCSI Manager from fully initializing,
     * which would change heap layout and cause crashes during INIT loading. */
    if (scsi.boot_mode) {
        return 0x00;
    }

    switch (reg) {
    case SCSI_REG_CSD:
        /* Current SCSI Data: reflects what's on the data bus.
         * During arbitration, reflects our own ID (from ODR).
         * Otherwise, bus idle = 0x00. */
        if (scsi.arb_state == 2) {
            /* After winning arbitration, data bus has our ID */
            val = scsi.odr;
        } else {
            val = 0x00;
        }
        break;

    case SCSI_REG_ICR: {
        /* Read ICR: bits 4-0 and 7 reflect written values.
         * Bit 6 = AIP (Arbitration In Progress)
         * Bit 5 = LA (Lost Arbitration)
         *
         * NCR 5380 arbitration sequence:
         * 1. Software sets MR.ARBITRATE and writes own ID to ODR
         * 2. Chip asserts BSY and puts ID on data bus
         * 3. AIP goes HIGH (arbitration in progress)
         * 4. After bus settle delay, chip checks for higher-priority IDs
         * 5. If won: AIP goes LOW, LA stays LOW
         * 6. If lost: AIP goes LOW, LA goes HIGH
         *
         * With no other devices, we always win after a short delay. */
        val = scsi.icr & 0x9F;  /* Written bits (mask out 6,5) */

        if (scsi.arb_state == 1) {
            /* Arbitration in progress — AIP=1 for a few reads */
            scsi.arb_reads++;
            if (scsi.arb_reads >= 3) {
                /* Arbitration won! Transition to won state */
                scsi.arb_state = 2;
                /* AIP=0, LA=0 (won) */
            } else {
                /* Still arbitrating — AIP=1 */
                val |= ICR_AIP;
            }
        }
        /* arb_state==0 or ==2: AIP=0, LA=0 (idle or won) */
        break;
    }

    case SCSI_REG_MR:
        val = scsi.mr;
        break;

    case SCSI_REG_TCR:
        val = scsi.tcr & 0x0F;  /* Only lower 4 bits are defined */
        break;

    case SCSI_REG_CSR:
        /* Current SCSI Bus Status: shows state of SCSI bus signal lines.
         * Reflects both initiator assertions (from ICR) and target
         * assertions (none — no devices present). */
        val = 0x00;
        /* During/after arbitration, initiator has asserted BSY */
        if (scsi.arb_state >= 1) val |= CSR_BSY;
        /* Reflect initiator's explicit signal assertions from ICR */
        if (scsi.icr & 0x80) val |= CSR_RST;
        if (scsi.icr & 0x08) val |= CSR_BSY;
        if (scsi.icr & 0x04) val |= CSR_SEL;
        /* No target ever responds — BSY from target side never set */
        break;

    case SCSI_REG_BSR:
        /* Bus and Status Register */
        val = 0x00;
        /* Phase match: compare TCR phase with CSR phase.
         * No device = no phase signals on bus. Match when TCR phase = 0. */
        {
            uint8_t tcr_phase = scsi.tcr & 0x07;
            if (tcr_phase == 0) {
                val |= BSR_PHASE_MATCH;
            }
        }
        break;

    case SCSI_REG_IDR:
        /* Input Data Register: latched data from last REQ.
         * No device = no data. */
        val = 0x00;
        break;

    case SCSI_REG_RPI:
        /* Reset Parity/Interrupt: reading clears parity+interrupt. */
        val = 0x00;
        break;
    }

    return val;
}

void scsi_write(uint32_t address, uint8_t value)
{
    int reg = scsi_reg(address);
    /* Boot mode: ignore writes to preserve pre-SCSI behavior */
    if (scsi.boot_mode) {
        return;
    }

    switch (reg) {
    case SCSI_REG_ODR:
        scsi.odr = value;
        break;

    case SCSI_REG_ICR:
        /* Bits: RST(7), [AIP(6) LA(5) r/o], ACK(4), BSY(3), SEL(2), ATN(1), DATA_BUS(0) */
        scsi.icr = value & 0x9F;  /* Only writable bits */
        break;

    case SCSI_REG_MR:
        scsi.mr = value;
        if (value & MR_ARBITRATE) {
            if (scsi.arb_state == 0) {
                /* Start arbitration: AIP goes high immediately */
                scsi.arb_state = 1;
                scsi.arb_reads = 0;
            }
        } else {
            /* Arbitrate mode cleared — reset state */
            scsi.arb_state = 0;
            scsi.arb_reads = 0;
        }
        break;

    case SCSI_REG_TCR:
        scsi.tcr = value & 0x0F;
        break;

    case SCSI_REG_SER:
        scsi.ser = value;
        break;

    case SCSI_REG_DMA_S:
    case SCSI_REG_DMA_T:
    case SCSI_REG_DMA_I:
        /* DMA triggers — no-op, no DMA hardware */
        break;
    }
}
