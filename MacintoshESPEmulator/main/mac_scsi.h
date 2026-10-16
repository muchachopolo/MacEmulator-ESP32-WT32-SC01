#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NCR 5380 SCSI Controller Emulation for Macintosh Classic
 *
 * The Mac Classic uses an NCR 5380 at $580000-$5FFFFF.
 * Register select: address bits A6-A4 → (address >> 4) & 0x07
 * This gives 8 registers spaced $10 apart.
 *
 * Minimal emulation: no devices on the bus. Arbitration succeeds
 * immediately, selection always times out (BSY never asserts).
 * This prevents apps like HD SC Setup from hanging.
 */

/* NCR 5380 Register indices */
#define SCSI_REG_ODR   0  /* Write: Output Data Register */
#define SCSI_REG_CSD   0  /* Read:  Current SCSI Data */
#define SCSI_REG_ICR   1  /* Initiator Command Register */
#define SCSI_REG_MR    2  /* Mode Register */
#define SCSI_REG_TCR   3  /* Target Command Register */
#define SCSI_REG_CSR   4  /* Read:  Current SCSI Bus Status */
#define SCSI_REG_SER   4  /* Write: Select Enable Register */
#define SCSI_REG_BSR   5  /* Read:  Bus and Status Register */
#define SCSI_REG_DMA_S 5  /* Write: Start DMA Send */
#define SCSI_REG_IDR   6  /* Read:  Input Data Register */
#define SCSI_REG_DMA_T 6  /* Write: Start DMA Target Receive */
#define SCSI_REG_RPI   7  /* Read:  Reset Parity/Interrupt */
#define SCSI_REG_DMA_I 7  /* Write: Start DMA Initiator Receive */

/* ICR read-back bits */
#define ICR_AIP        0x40  /* Arbitration In Progress */
#define ICR_LA         0x20  /* Lost Arbitration */

/* CSR bits */
#define CSR_RST        0x80
#define CSR_BSY        0x40
#define CSR_REQ        0x20
#define CSR_MSG        0x10
#define CSR_CD         0x08
#define CSR_IO         0x04
#define CSR_SEL        0x02
#define CSR_DBP        0x01

/* BSR bits */
#define BSR_END_DMA    0x80
#define BSR_DMA_REQ    0x40
#define BSR_PARITY_ERR 0x20
#define BSR_IRQ        0x10
#define BSR_PHASE_MATCH 0x08
#define BSR_BUSY_ERR   0x04
#define BSR_ATN        0x02
#define BSR_ACK        0x01

/* MR bits */
#define MR_ARBITRATE   0x01
#define MR_DMA_MODE    0x02

void scsi_init(void);
uint8_t scsi_read(uint32_t address);
void scsi_write(uint32_t address, uint8_t value);

/* Enable full register emulation (call after boot completes).
 * During boot, all reads return $00 to match pre-SCSI behavior
 * and avoid heap layout changes from SCSI Manager init. */
void scsi_set_active(bool active);

#ifdef __cplusplus
}
#endif
