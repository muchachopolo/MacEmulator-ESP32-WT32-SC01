/*
 * VIA 6522 Emulation for Macintosh Classic
 */
#include "mac_via.h"
#include "mac_emulator.h"   /* CLASSIC_NATIVE_LCD (default of CLASSIC_VIA_ECLOCK) */
#include "m68k.h"           /* m68k_cycles_run() for mid-slice timer reads */
#if MAC_ROM_LC_II
#include "mac_egret.h"
#include "mac_v8video.h"
#endif
#include <string.h>
#include <esp_log.h>

static const char *TAG = "via";

typedef struct {
    uint8_t ora;        /* Output Register A */
    uint8_t orb;        /* Output Register B */
    uint8_t ira;        /* Input Register A */
    uint8_t irb;        /* Input Register B */
    uint8_t ddra;       /* Data Direction A */
    uint8_t ddrb;       /* Data Direction B */
    uint8_t acr;        /* Auxiliary Control Register */
    uint8_t pcr;        /* Peripheral Control Register */
    uint8_t ifr;        /* Interrupt Flag Register */
    uint8_t ier;        /* Interrupt Enable Register */
    uint8_t sr;         /* Shift Register */

    uint16_t t1_counter;
    uint16_t t1_latch;
    uint16_t t2_counter;
    uint8_t  t2_latch_low;
    bool t1_armed;          /* T1 fires on next underflow (one-shot mode) */
    bool t2_armed;          /* T2 fires on next underflow (always one-shot) */

    int sr_pending;         /* >0: countdown ticks until SR interrupt fires */
    int e_rem;              /* V8: CPU cycles not yet turned into E-clock ticks */

    bool overlay;           /* ROM overlay at address 0 */
    bool overlay_was_off;   /* True once overlay has been OFF (vectors copied to RAM) */
    bool adb_active;        /* True after boot reaches INIT phase - enables ADB SR triggers */

    /* Debug counters (per diagnostic period) */
    uint32_t t2_fire_count;     /* Times T2 underflowed in via_tick */
    uint32_t t2ch_write_count;  /* Times T2CH was written by handler */
    uint32_t t2cl_read_count;   /* Times T2CL was read (clears IFR T2!) */

    /* Per-register access counters for diagnostics */
    uint32_t read_counts[16];
    uint32_t write_counts[16];
} via_state_t;

static via_state_t via;

/* Exposed for fast memory dispatch (avoids function call per access) */
bool via_overlay_fast = true;
bool via_overlay_was_off_fast = false;

void via_init(void)
{
    memset(&via, 0, sizeof(via));
    via.overlay = true;     /* Overlay enabled at startup */
    via.ora = 0x7F;         /* Port A default - overlay bit set */
    via.ddra = 0x00;
    via.ddrb = 0x00;
    /*
     * Input register defaults:
     * PA0 = must be 1 or ROM enters serial debug monitor (diagnostic detect)
     * PA3 = /SCC.IRQ (active low) — must be 1 for "no SCC interrupt"
     * PB0 = RTC data (pulled up when idle) — set to 1
     * PB4+PB5 = ADB bus state: $30 = "data ready/idle". Without these bits,
     *           system INITs polling (ORB & $30)==$30 for ADB bus readiness
     *           loop forever at ~F5100 (PC=$0794C4). The subsequent VIA SR
     *           read gets 0 (no keyboard data), which INITs handle gracefully.
     */
    via.ira = 0x0F;     /* PA0-PA2 high (no diag device), PA3 high (no SCC IRQ) */
    via.irb = 0x31;     /* PB0 high (RTC), PB3 low (button down — safe for boot), PB4+PB5 high (ADB idle) */
#if MAC_ROM_LC_II
    via.irb = 0x39;     /* V8: PB3 = Egret/Cuda TREQ (active low) idle high; no mouse button on VIA */
    /* V8 model ID on PA6/PA4/PA2/PA1 (ROM $A047C6, matched against
     * ProductInfo $20/$24): Color Classic = 0/0/0/1 -> ProductInfo $A03858.
     * PA0 and PA3 stay high as on the Classic.
     * LC / LC II (same ROM; ProductInfo $A03BA6, BoxFlag $0D) = 1/1/1/0.
     * Selected by MAC_V8_CUDA, the same switch as the Egret/Cuda HLE. */
#if MAC_V8_CUDA
    via.ira = 0x0B;
#else
    via.ira = 0x5D;
#endif
    egret_init();
#endif
    via.t1_latch = 0xFFFF;
    via.t1_counter = 0xFFFF;
    ESP_LOGI(TAG, "VIA initialized, overlay enabled");
}

void via_reset(void)
{
    via_init();
}

void via_set_mouse_button(bool pressed)
{
#if MAC_ROM_LC_II
    (void)pressed;      /* V8: PB3 is Egret TREQ; the button comes via ADB */
    return;
#endif
    /* PB3 = mouse button, active low: 0=pressed, 1=released */
    if (pressed) {
        via.irb &= ~0x08;
    } else {
        via.irb |= 0x08;
    }
}

static void via_update_irq(void)
{
    if (via.ifr & via.ier & 0x7F) {
        via.ifr |= VIA_IRQ_ANY;    /* Set bit 7 if any enabled interrupt is pending */
    } else {
        via.ifr &= ~VIA_IRQ_ANY;
    }
#if MAC_ROM_LC_II
    /* Drive the CPU IRQ line immediately rather than at the next slice
     * boundary.  Otherwise a level-1 request that the ROM has already
     * cleared (IFR/IER/T2CH write) is still taken as soon as it lowers
     * the SR mask: the LC II TimeDBRA calibration at $A00800 then counts
     * 0 DBRAs and the boot dies on DIVU #0 at $A43A8C (Sad Mac 0F/0004). */
    via_update_cpu_irq();
#endif
}

#if MAC_ROM_LC_II
/* V8 CPU interrupt level: pseudo-VIA2 (slot VBL) = 2, VIA1 = 1 */
void via_update_cpu_irq(void)
{
    int lvl = v8video_irq_pending() ? M68K_IRQ_2 :
              (via.ifr & VIA_IRQ_ANY) ? M68K_IRQ_1 : M68K_IRQ_NONE;
    m68k_set_irq(lvl);
}
#endif

/* Timer read-back. via_tick() only runs between execution slices (up to
 * 4000 cycles), so a counter read mid-slice would be frozen.  On V8 the
 * System's Time Manager calibration (InsTime/PrimeTime, ~3000-cycle DBRA
 * loops, RmvTime) then measures 0 elapsed and divides by zero (bomb box
 * "divide by zero" right after "Welcome to Macintosh").  Interpolate with
 * the E-clock ticks (CPU/10, see via_tick) already run in the current
 * slice; only costs on timer reads. */
/* Classic timer model.
 *  CLASSIC_VIA_INTERP (default 1): timer reads interpolate the cycles already
 *    run in the current slice, and a counter load mid-slice is pre-biased by
 *    them (the next via_tick() subtracts the whole slice).
 *  CLASSIC_VIA_ECLOCK (default = CLASSIC_NATIVE_LCD): timers count at the E
 *    clock (CPU/10) like the real Classic / V8, instead of 1 per CPU cycle.
 * Why: a System Time Manager calibration (RAM code, 10x {PrimeTime; 4x
 * TimeDBRA DBRA loops; RmvTime}, then DIVU by the shortest time and, later,
 * DIVU by result/100) divides by zero unless timer readings are both
 * non-zero (needs INTERP) and on the real time scale (needs ECLOCK: with the
 * 10x-fast timers TimeDBRA calibrates to $44 and result/100 == 0). The old
 * slice-frozen, 10x-fast model only survived by slice-phase luck; the native
 * 480x320 patches shift the phase and it crashed at F5230. Both 0 = the
 * historical model (output byte-identical to the pre-change build). */
#ifndef CLASSIC_VIA_ECLOCK
#define CLASSIC_VIA_ECLOCK CLASSIC_NATIVE_LCD
#endif
#define VIA_ECLOCK (MAC_ROM_LC_II || CLASSIC_VIA_ECLOCK)
#if MAC_ROM_LC_II
#define VIA_T_NOW(c) ((uint16_t)((c) - (uint16_t)((via.e_rem + m68k_cycles_run()) / 10)))
#define VIA_T_LOAD(v) (v)
#else
#ifndef CLASSIC_VIA_INTERP
#define CLASSIC_VIA_INTERP 1
#endif
#if CLASSIC_VIA_INTERP
#if CLASSIC_VIA_ECLOCK
#define VIA_RUN_TICKS() ((uint32_t)((via.e_rem + m68k_cycles_run()) / 10))
#else
#define VIA_RUN_TICKS() ((uint32_t)m68k_cycles_run())
#endif
#define VIA_T_NOW(c)  ((uint16_t)((c) - (uint16_t)VIA_RUN_TICKS()))
static inline uint16_t via_t_load(uint16_t v)
{
    uint32_t b = (uint32_t)v + VIA_RUN_TICKS();
    return (b > 0xFFFF) ? v : (uint16_t)b;   /* never wrap a fresh load */
}
#define VIA_T_LOAD(v) via_t_load(v)
#else
#define VIA_T_NOW(c)  (c)
#define VIA_T_LOAD(v) (v)
#endif
#endif

uint8_t via_read(uint32_t address)
{
    /* VIA registers at 0xEFE1FE, spaced 0x200 apart, accessible via odd bytes
     * Register index = (address >> 9) & 0xF */
    int reg = (address >> 9) & 0xF;
    via.read_counts[reg]++;

    switch (reg) {
    case VIA_REG_ORB: {
        /* Port B: bits driven by DDRB come from ORB, others from IRB */
        uint8_t val = (via.orb & via.ddrb) | (via.irb & ~via.ddrb);
        /* Force PB4+PB5 high: on real Mac Classic, the ADB microcontroller
         * (Egret/SWIM) drives state bits ST0/ST1 to $30 when idle/ready.
         * Without this, INITs polling (ORB & $30)==$30 stall forever
         * because the bits are configured as outputs (via DDRB) and the
         * CPU wrote 0, but expects the ADB controller to pull them high. */
#if !MAC_ROM_LC_II
        val |= 0x30;
#endif
        /* Clear CB1/CB2 flags on read */
        via.ifr &= ~(VIA_IRQ_CB1 | VIA_IRQ_CB2);
        via_update_irq();
        return val;
    }

    case VIA_REG_ORA:
    case VIA_REG_ORA_NH: {
        /* Port A: overlay is bit 4 of output */
        uint8_t val = (via.ora & via.ddra) | (via.ira & ~via.ddra);
        if (reg == VIA_REG_ORA) {
            via.ifr &= ~(VIA_IRQ_CA1 | VIA_IRQ_CA2);
            via_update_irq();
        }
        return val;
    }

    case VIA_REG_DDRB: return via.ddrb;
    case VIA_REG_DDRA: return via.ddra;

    case VIA_REG_T1CL:
        via.ifr &= ~VIA_IRQ_T1;
        via_update_irq();
        return VIA_T_NOW(via.t1_counter) & 0xFF;

    case VIA_REG_T1CH:
        return (VIA_T_NOW(via.t1_counter) >> 8) & 0xFF;

    case VIA_REG_T1LL:
        return via.t1_latch & 0xFF;

    case VIA_REG_T1LH:
        return (via.t1_latch >> 8) & 0xFF;

    case VIA_REG_T2CL:
        via.t2cl_read_count++;
        via.ifr &= ~VIA_IRQ_T2;
        via_update_irq();
        return VIA_T_NOW(via.t2_counter) & 0xFF;

    case VIA_REG_T2CH:
        return (VIA_T_NOW(via.t2_counter) >> 8) & 0xFF;

    case VIA_REG_SR:
        via.ifr &= ~VIA_IRQ_SR;
        via_update_irq();
        return via.sr;

    case VIA_REG_ACR: return via.acr;
    case VIA_REG_PCR: return via.pcr;

    case VIA_REG_IFR:
        return via.ifr;

    case VIA_REG_IER:
        return via.ier | 0x80;  /* Bit 7 always reads as 1 */

    default:
        return 0;
    }
}

void via_write(uint32_t address, uint8_t value)
{
    int reg = (address >> 9) & 0xF;
    via.write_counts[reg]++;

    switch (reg) {
    case VIA_REG_ORB:
        via.orb = value;
        via.ifr &= ~(VIA_IRQ_CB1 | VIA_IRQ_CB2);
        via_update_irq();
#if MAC_ROM_LC_II
        egret_portb_changed(via.orb, via.ddrb);
#endif
        break;

    case VIA_REG_ORA:
    case VIA_REG_ORA_NH: {
        via.ora = value;
        /* Mac overlay uses a one-shot latch: writing to VIA ORA for the
         * first time clears the overlay flip-flop permanently. On real
         * hardware, the overlay only controls reads (ROM vs RAM at $0).
         * The first ORA write happens after vectors are copied to RAM. */
#if !MAC_ROM_LC_II
        if (via.overlay) {
            ESP_LOGI(TAG, "Overlay cleared by ORA write (ORA=0x%02X)", value);
            via_clear_overlay();
        }
#endif
        if (reg == VIA_REG_ORA) {
            via.ifr &= ~(VIA_IRQ_CA1 | VIA_IRQ_CA2);
            via_update_irq();
        }
        break;
    }

    case VIA_REG_DDRB:
        via.ddrb = value;
#if MAC_ROM_LC_II
        egret_portb_changed(via.orb, via.ddrb);
#endif
        break;

    case VIA_REG_DDRA: {
        via.ddra = value;
        /* Overlay is one-shot (cleared by first ORA write), so DDRA
         * changes don't affect it. */
        break;
    }

    case VIA_REG_T1CL:
    case VIA_REG_T1LL:
        via.t1_latch = (via.t1_latch & 0xFF00) | value;
        break;

    case VIA_REG_T1CH:
        via.t1_latch = (via.t1_latch & 0x00FF) | ((uint16_t)value << 8);
        via.t1_counter = VIA_T_LOAD(via.t1_latch);
        via.t1_armed = true;
        via.ifr &= ~VIA_IRQ_T1;
        via_update_irq();
        break;

    case VIA_REG_T1LH:
        via.t1_latch = (via.t1_latch & 0x00FF) | ((uint16_t)value << 8);
        via.ifr &= ~VIA_IRQ_T1;
        via_update_irq();
        break;

    case VIA_REG_T2CL:
        via.t2_latch_low = value;
        break;

    case VIA_REG_T2CH:
        via.t2ch_write_count++;
        via.t2_counter = VIA_T_LOAD(((uint16_t)value << 8) | via.t2_latch_low);
        via.t2_armed = true;
        via.ifr &= ~VIA_IRQ_T2;
        via_update_irq();
        break;

    case VIA_REG_SR:
        via.sr = value;
        via.ifr &= ~VIA_IRQ_SR;
        via_update_irq();
        /* If shift register mode is active AND we're in the ADB phase (past
         * early boot), schedule a SR interrupt to simulate the shift
         * completing. During early boot, the ROM uses VIA SR for SCC
         * handshake and does NOT expect SR interrupts. */
        if ((via.acr & 0x1C) && via.adb_active && !MAC_ROM_LC_II) {
            via.sr_pending = 200;
            {
                static int sr_write_log = 0;
                if (sr_write_log < 10) {
                    sr_write_log++;
                    ESP_LOGW(TAG, "SR-WRITE #%d: val=%02X ACR=%02X IER=%02X mode=%d",
                             sr_write_log, value, via.acr, via.ier, (via.acr >> 2) & 7);
                }
            }
        }
        break;

    case VIA_REG_ACR: {
        uint8_t old_mode = (via.acr >> 2) & 0x07;
        uint8_t new_mode = (value >> 2) & 0x07;
        via.acr = value;
        /* If shift register mode changed to an active mode, schedule a SR
         * interrupt. This handles the ADB protocol: after command shift-out
         * completes, the handler switches ACR from shift-out (mode 7) to
         * shift-in (mode 1-3) to receive the device response. The SR
         * interrupt must fire again for the response phase. */
        if (new_mode != 0 && new_mode != old_mode && via.adb_active && !MAC_ROM_LC_II) {
            via.sr_pending = 200;
        }
        break;
    }

    case VIA_REG_PCR:
        via.pcr = value;
        break;

    case VIA_REG_IFR:
        /* Writing 1 to a bit clears that interrupt flag */
        via.ifr &= ~(value & 0x7F);
        via_update_irq();
        break;

    case VIA_REG_IER:
        if (value & 0x80) {
            /* Setting bits: OR with mask */
            via.ier |= (value & 0x7F);
        } else {
            /* Clearing bits: AND with complement of mask */
            via.ier &= ~(value & 0x7F);
        }
        via_update_irq();
        break;
    }
}

void via_vblank(void)
{
    /* CA1 is connected to VBlank - set the interrupt flag */
    via.ifr |= VIA_IRQ_CA1;
    via_update_irq();
}

void via_one_second(void)
{
    /* CA2 is connected to the one-second interrupt from the RTC */
    via.ifr |= VIA_IRQ_CA2;
    via_update_irq();
#if MAC_ROM_LC_II
    egret_one_second();
#endif
}

void via_tick(int cycles)
{
#if MAC_ROM_LC_II
    egret_tick(cycles);
#endif
    /* Shift Register completion: simulate ADB shift finishing */
    if (via.sr_pending > 0) {
        via.sr_pending -= cycles;
        if (via.sr_pending <= 0) {
            via.sr_pending = 0;
            /* Shift complete: set SR interrupt flag */
            via.ifr |= VIA_IRQ_SR;
            /* If shifting IN (ACR bits 4-2 = 001,010,011), set SR to 0xFF
             * to simulate "no device responded" (bus idle = all 1s) */
            uint8_t sr_mode = (via.acr >> 2) & 0x07;
            if (sr_mode >= 1 && sr_mode <= 3) {
                via.sr = 0xFF;  /* No ADB device: timeout / idle bus */
            }
            via_update_irq();
            {
                static int sr_fire_count = 0;
                if (sr_fire_count < 10) {
                    sr_fire_count++;
                    ESP_LOGW(TAG, "SR-FIRE #%d: IFR=%02X IER=%02X ACR=%02X mode=%d pending=%s SR=%02X",
                             sr_fire_count, via.ifr, via.ier, via.acr, sr_mode,
                             (via.ifr & via.ier & 0x7F) ? "YES" : "NO", via.sr);
                }
            }
        }
    }

#if VIA_ECLOCK
    /* V8: VIA1 timers count at the E clock = CPU clock / 10 (783.36 kHz),
     * like the real LC II.  (The Classic build keeps its historical
     * 1-per-CPU-cycle rate.)  With the 10x-fast rate, the Time Manager's
     * T2 reload loop at $A0B090 never sees a positive remainder once
     * timer reads advance mid-slice, and spins forever at SR=$2700. */
    {
        int total = cycles + via.e_rem;
        cycles = total / 10;
        via.e_rem = total - cycles * 10;
    }
#endif
    /* Timer 1 */
    int old_t1 = via.t1_counter;
    via.t1_counter -= cycles;

    if (via.t1_counter > old_t1) {
        /* Timer 1 underflowed */
        if (via.acr & 0x40) {
            /* Free-running mode: always fires, reload from latch */
            via.ifr |= VIA_IRQ_T1;
            via.t1_counter = via.t1_latch;
        } else {
            /* One-shot mode: only fire if armed (first underflow after T1CH write) */
            if (via.t1_armed) {
                via.ifr |= VIA_IRQ_T1;
                via.t1_armed = false;
            }
        }
        via_update_irq();
    }

    /* Timer 2 (one-shot only; counts clock pulses when ACR bit 5 is 0) */
    if (!(via.acr & 0x20)) {
        int old_t2 = via.t2_counter;
        via.t2_counter -= cycles;
        if (via.t2_counter > old_t2) {
            /* Timer 2 underflowed */
            via.t2_fire_count++;
            if (via.t2_armed) {
                via.ifr |= VIA_IRQ_T2;
                via.t2_armed = false;
            }
            via_update_irq();
        }
    }
}

bool via_irq_pending(void)
{
    return (via.ifr & via.ier & 0x7F) != 0;
}

/* Classic: first ORA write. V8: first access to the ROM's real address
 * window $A00000 (ORA writes leave the overlay alone there). */
void via_clear_overlay(void)
{
    via.overlay = false;
    via.overlay_was_off = true;
    via_overlay_fast = false;
    via_overlay_was_off_fast = true;
}

bool via_get_overlay(void)
{
    return via.overlay;
}

bool via_get_overlay_was_off(void)
{
    return via.overlay_was_off;
}

void via_egret_set_treq(bool asserted)
{
    if (asserted) via.irb &= ~0x08;
    else          via.irb |= 0x08;
}

void via_egret_shift_done(bool in, uint8_t byte)
{
    if (in) via.sr = byte;
    via.ifr |= VIA_IRQ_SR;
    via_update_irq();
}

uint8_t via_get_sr(void) { return via.sr; }
uint8_t via_get_ifr(void) { return via.ifr; }
uint8_t via_get_ier(void) { return via.ier; }
uint8_t via_get_acr(void) { return via.acr; }
uint8_t via_get_ora(void) { return via.ora; }
uint16_t via_get_t1(void) { return via.t1_counter; }
uint16_t via_get_t2(void) { return via.t2_counter; }

/* Debug counter accessors */
uint32_t via_get_t2_fires(void) { return via.t2_fire_count; }
uint32_t via_get_t2ch_writes(void) { return via.t2ch_write_count; }
uint32_t via_get_t2cl_reads(void) { return via.t2cl_read_count; }
uint16_t via_get_t2_peak_pending(void) { return 0; }
const uint32_t *via_get_read_counts(void) { return via.read_counts; }
const uint32_t *via_get_write_counts(void) { return via.write_counts; }
void via_set_adb_active(bool active) {
    if (active && !via.adb_active) {
        ESP_LOGI(TAG, "ADB SR interrupt emulation enabled");
    }
    via.adb_active = active;
}

void via_reset_debug_counters(void) {
    via.t2_fire_count = 0;
    via.t2ch_write_count = 0;
    via.t2cl_read_count = 0;
    memset(via.read_counts, 0, sizeof(via.read_counts));
    memset(via.write_counts, 0, sizeof(via.write_counts));
}
