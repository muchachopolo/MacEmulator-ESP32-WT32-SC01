/*
 * SCC (Zilog 8530) Stub for Macintosh Classic
 *
 * Implements register pointer mechanism and returns safe defaults.
 * The Z8530 has two channels (A and B), each with read registers (RR0-RR15)
 * and write registers (WR0-WR15). A write to WR0 sets the register pointer
 * for the next read/write, which then resets to 0.
 *
 * The Mac Classic ROM sends "*APPLE*" on channel A (modem port) and waits
 * for a "*S" handshake response. Without it, the ROM loops forever in the
 * dispatch table at 0x402264. We inject "*S" after the ROM starts polling
 * to satisfy the handshake and let boot continue.
 */
#include "mac_scc.h"
#include <esp_log.h>

static const char *TAG = "scc";

#define SCC_NUM_CHANNELS 2
#define SCC_CHAN_A       1
#define SCC_CHAN_B       0

static int scc_log_count = 0;
#define SCC_LOG_LIMIT 5

static uint32_t scc_read_counter = 0;

typedef struct {
    uint8_t rr[16];         /* Read registers */
    uint8_t wr[16];         /* Write registers */
    uint8_t reg_ptr;        /* Register pointer (set by WR0 bits 2-0) */
} scc_channel_t;

static scc_channel_t scc_chan[SCC_NUM_CHANNELS];

/* Rx data buffer for potential future serial emulation */
#define RX_BUF_SIZE 8
static uint8_t rx_buf[RX_BUF_SIZE];
static int rx_head = 0;
static int rx_tail = 0;
static int rx_count = 0;
static uint32_t ch_a_poll_count = 0;  /* Count RR0 reads on channel A */

static void rx_enqueue(uint8_t byte)
{
    if (rx_count < RX_BUF_SIZE) {
        rx_buf[rx_head] = byte;
        rx_head = (rx_head + 1) % RX_BUF_SIZE;
        rx_count++;
    }
}

static uint8_t rx_dequeue(void)
{
    if (rx_count > 0) {
        uint8_t byte = rx_buf[rx_tail];
        rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
        rx_count--;
        return byte;
    }
    return 0x00;
}

void scc_init(void)
{
    for (int ch = 0; ch < SCC_NUM_CHANNELS; ch++) {
        for (int i = 0; i < 16; i++) {
            scc_chan[ch].rr[i] = 0;
            scc_chan[ch].wr[i] = 0;
        }
        scc_chan[ch].reg_ptr = 0;

        /* RR0: Tx Buffer Empty + Tx Underrun + DCD + CTS + Sync/Hunt
         * (bits 2,3,4,5,6)
         * On a real Mac with no serial cable, DCD and CTS pins float HIGH
         * (inactive). RR0 bits 3 and 5 reflect the pin state: 1 = inactive.
         * Without DCD=1, the ROM thinks a carrier is present and polls forever.
         * Bit 4 (Sync/Hunt) must be set: in SDLC mode (LocalTalk) the SCC
         * starts in hunt mode after reset. INITs polling for LocalTalk
         * readiness check (RR0 & $30) == $30, which requires both CTS and
         * Sync/Hunt. Without bit 4, the INIT loops forever at ~F5100. */
        scc_chan[ch].rr[0] = 0x7C;
        /* RR1: All Sent (bit 0) + residue code */
        scc_chan[ch].rr[1] = 0x07;
        /* RR3: no interrupts pending */
        scc_chan[ch].rr[3] = 0x00;
        /* RR10: no errors */
        scc_chan[ch].rr[10] = 0x00;
    }

    /* Reset Rx buffer state */
    rx_head = rx_tail = rx_count = 0;
    ch_a_poll_count = 0;
}

uint32_t scc_get_read_count(void) { return scc_read_counter; }
void scc_reset_read_count(void) { scc_read_counter = 0; }

uint8_t scc_read(uint32_t address)
{
    scc_read_counter++;

    /*
     * SCC read address decoding (Mac Classic):
     * Bit 1 selects channel: 0 = channel B, 1 = channel A
     * Bit 2 selects type: 0 = control, 1 = data
     */
    int channel = (address >> 1) & 1;
    int is_data = (address >> 2) & 1;

    if (is_data) {
        /* Data read: return queued Rx byte if available */
        if (channel == SCC_CHAN_A && rx_count > 0) {
            uint8_t byte = rx_dequeue();
            /* Update RR0: clear Rx Available if buffer empty */
            if (rx_count == 0) {
                scc_chan[SCC_CHAN_A].rr[0] &= ~0x01;
            }
            ESP_LOGI(TAG, "Rx ch A data=0x%02X ('%c') remaining=%d",
                     byte, (byte >= 0x20 && byte < 0x7F) ? byte : '.', rx_count);
            return byte;
        }
        return 0x00;    /* No data available */
    }

    /* Control read: return the register selected by the pointer */
    scc_channel_t *ch = &scc_chan[channel];
    uint8_t reg = ch->reg_ptr;
    ch->reg_ptr = 0;   /* Reset pointer to 0 after read (Z8530 behavior) */

    /* Track channel A RR0 polls. Do NOT inject *S handshake — that
     * puts the ROM in service mode (waits for debugger commands forever).
     * Instead, let the dispatch loop time out naturally via VIA timer. */
    if (channel == SCC_CHAN_A && reg == 0) {
        ch_a_poll_count++;
        if (ch_a_poll_count == 10000 || ch_a_poll_count == 50000 ||
            ch_a_poll_count == 200000) {
            ESP_LOGI(TAG, "ChA RR0 polls: %lu (no injection)",
                     (unsigned long)ch_a_poll_count);
        }
    }

    uint8_t val = ch->rr[reg & 0x0F];
    if (scc_log_count < SCC_LOG_LIMIT) {
        scc_log_count++;
        ESP_LOGI(TAG, "R ch%c RR%d=0x%02X", channel ? 'A' : 'B', reg, val);
    }
    return val;
}

void scc_write(uint32_t address, uint8_t value)
{
    int channel = (address >> 1) & 1;
    int is_data = (address >> 2) & 1;

    scc_channel_t *ch = &scc_chan[channel];

    if (is_data) {
        /* Data write - ignore (no actual serial output) */
        return;
    }

    /* Control write */
    uint8_t reg = ch->reg_ptr;

    if (reg == 0) {
        /* WR0: register pointer and command */
        uint8_t new_ptr = value & 0x07;       /* bits 2-0: register select */
        uint8_t cmd = (value >> 3) & 0x07;    /* bits 5-3: command code */

        /* Handle point-high bit (select registers 8-15) */
        if (cmd == 1) {
            new_ptr |= 0x08;  /* Point high: add 8 to register number */
        }
        /* Other commands (reset, error reset, etc.) - accept silently */

        if (scc_log_count < SCC_LOG_LIMIT) {
            scc_log_count++;
            ESP_LOGI(TAG, "W ch%c WR0=0x%02X (ptr=%d cmd=%d)", channel ? 'A' : 'B', value, new_ptr, cmd);
        }
        ch->reg_ptr = new_ptr;
    } else {
        /* Writing to WRn (n>0) */
        if (scc_log_count < SCC_LOG_LIMIT) {
            scc_log_count++;
            ESP_LOGI(TAG, "W ch%c WR%d=0x%02X", channel ? 'A' : 'B', reg, value);
        }
        ch->wr[reg & 0x0F] = value;

        /* Handle specific register writes */
        if (reg == 9) {
            /* WR9: Master interrupt control / hardware reset */
            if (value & 0xC0) {
                /* Channel reset or hardware reset - re-init affected channels */
                if ((value & 0xC0) == 0xC0) {
                    /* Force hardware reset - reset both channels */
                    scc_init();
                } else if (value & 0x80) {
                    /* Reset channel A */
                    scc_chan[SCC_CHAN_A].reg_ptr = 0;
                } else if (value & 0x40) {
                    /* Reset channel B */
                    scc_chan[SCC_CHAN_B].reg_ptr = 0;
                }
            }
        }

        /* Reset pointer after write to any register other than WR0 */
        ch->reg_ptr = 0;
    }
}
