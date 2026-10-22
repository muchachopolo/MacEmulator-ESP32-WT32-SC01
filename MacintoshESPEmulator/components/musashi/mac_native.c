/* ======================================================================== */
/* mac_native.c - native (C) replacements for hot Mac OS 68k routines      */
/* (plan step 3b, spec/next-steps-plan.md). Lives in the Musashi component */
/* because it works on Musashi's internal CPU state (m68kcpu.h) and gets   */
/* the component's warning suppressions.                                    */
/*                                                                          */
/* 1. MAC_NATIVE_TRAPDISP: the A-line trap dispatcher.                       */
/*    Called from m68ki_exception_1010() right after Musashi has built the   */
/*    exception frame and jumped through the A-line vector (A-line path     */
/*    only; nothing is added to the per-instruction path). If the vector    */
/*    target holds one of the two known dispatchers, byte-for-byte:         */
/*      - Classic + System 7.x: the System's RAM dispatcher (68000 frame,   */
/*        512+512 toolbox entries: $0E00 and the table at ($0664)),         */
/*      - LC II ROM $A099B0 (68020+ frame, 1024-entry toolbox table $0E00), */
/*    the dispatcher's own instructions are executed here as C, one 68k     */
/*    instruction at a time with Musashi's exact semantics: same memory     */
/*    accesses through the same m68ki_read/write functions (stack writes,   */
/*    trap-table reads, the trap-word read through the stacked PC), same    */
/*    register and flag results (Musashi's flag macros), same cycle charges */
/*    (CYC_INSTRUCTION[opcode] + not-taken branch + 68020 full-format index */
/*    extras). Before each instruction the remaining-cycle check of         */
/*    m68k_execute()'s loop is repeated, so a time slice ends on exactly    */
/*    the same instruction as in the interpreter (the CPU is then left in   */
/*    the middle of the dispatcher and the interpreter continues). The      */
/*    result is therefore identical to interpreting the dispatcher, only    */
/*    without the fetch/decode work: the toolbox path ends at the handler,  */
/*    the OS path ends after its JSR to the handler (the short epilogue     */
/*    after the handler returns is interpreted).                            */
/*    The RAM dispatcher is re-verified (memcmp of its 218 bytes) on every  */
/*    trap, so a patched or moved dispatcher falls back to the interpreter. */
/*    Byte-identical to MAC_NATIVE_TRAPDISP=0 (RAM dumps and screenshots,   */
/*    host harness, both ROMs and both boot disks).                          */
/* 2. MAC_NATIVE_DCMP: resource decompressor 'dcmp' 3 (see section 2).      */
/*    Output-exact (every call checked against the 68k original), but it    */
/*    takes no emulated time, so it changes the timeline (boot is shorter). */
/* 3. MAC_NATIVE_LOOPS: long ROM loops entered at time-slice start (see     */
/*    section 3). Byte-identical to MAC_NATIVE_LOOPS=0.                      */
/* Switches and hooks: m68kconf.h. Host counters: "H native:" lines at exit. */
/* ======================================================================== */

#include <string.h>
#include <stdint.h>
#include "m68kcpu.h"

/* main/mac_memory.h (not on this component's include path) */
extern uint8_t  *mac_mem_get_ram(void);
extern uint32_t  mac_mem_get_ram_size(void);

#if MAC_NATIVE_TRAPDISP

/* Classic, System 7.5.x RAM dispatcher (found at $70DE). 218 bytes:
 * entry, toolbox paths, alternate entry (+$7A, unused here), OS paths. */
static const unsigned char nd_classic_img[218] = {
    0x3F,0x03,0x2F,0x0A,0x24,0x6F,0x00,0x08,0x36,0x1A,0x0C,0x43,0xA8,0x00,0x65,0x7A,
    0xD6,0x43,0xD6,0x43,0x04,0x43,0xA8,0x00,0x64,0x14,0x2F,0x4A,0x00,0x08,0x34,0x43,
    0x36,0x2F,0x00,0x04,0x2F,0x6A,0x16,0x00,0x00,0x04,0x24,0x5F,0x4E,0x75,0x06,0x43,
    0x08,0x00,0x08,0x83,0x00,0x0C,0x67,0x18,0x08,0x83,0x00,0x0B,0x67,0x2C,0x34,0x43,
    0xD5,0xF8,0x06,0x64,0x2F,0x52,0x00,0x08,0x24,0x5F,0x36,0x17,0x58,0x4F,0x4E,0x75,
    0x08,0x83,0x00,0x0B,0x2F,0x4A,0x00,0x08,0x34,0x43,0x36,0x2F,0x00,0x04,0xD5,0xF8,
    0x06,0x64,0x2F,0x52,0x00,0x04,0x24,0x5F,0x4E,0x75,0x34,0x43,0x2F,0x6A,0x0E,0x00,
    0x00,0x08,0x24,0x5F,0x36,0x17,0x58,0x4F,0x4E,0x75,0x3E,0x83,0x2F,0x0A,0x24,0x6F,
    0x00,0x06,0x36,0x1A,0x0C,0x43,0xA8,0x00,0x64,0x86,0x2F,0x4A,0x00,0x08,0x2F,0x02,
    0x2F,0x01,0x2F,0x09,0x32,0x03,0x02,0x43,0x01,0x00,0x66,0x22,0x2F,0x08,0x16,0x01,
    0xD6,0x43,0xD6,0x43,0x34,0x43,0x36,0x2F,0x00,0x14,0x24,0x6A,0x04,0x00,0x4E,0x92,
    0x20,0x5F,0x22,0x5F,0x4C,0xDF,0x04,0x06,0x4A,0x40,0x58,0x4F,0x4E,0x75,0x16,0x01,
    0xD6,0x43,0xD6,0x43,0x34,0x43,0x36,0x2F,0x00,0x10,0x24,0x52,0x4E,0x92,0x22,0x5F,
    0x4C,0xDF,0x04,0x06,0x4A,0x40,0x58,0x4F,0x4E,0x75
};

/* LC II ROM dispatcher at $A099B0 (132 bytes incl. alignment padding). */
static const unsigned char nd_lc2_img[132] = {
    0x2F,0x0A,0x2F,0x02,0x24,0x6F,0x00,0x0A,0x34,0x1A,0x0C,0x42,0xA8,0x00,0x65,0x30,
    0x04,0x42,0xAC,0x00,0x64,0x1A,0x2F,0x70,0x25,0xA0,0x1E,0x00,0x00,0x08,0x2F,0x4A,
    0x00,0x0C,0x24,0x1F,0x24,0x5F,0x4E,0x75,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x2F,0x70,0x25,0xA0,0x0E,0x00,0x00,0x08,0x24,0x1F,0x24,0x5F,0x4E,0x74,0x00,0x04,
    0x2F,0x01,0x2F,0x09,0x32,0x02,0x2F,0x4A,0x00,0x14,0x02,0x42,0x01,0x00,0x66,0x20,
    0x14,0x01,0x2F,0x08,0x4E,0xB0,0x25,0xA1,0x04,0x00,0x20,0x5F,0x22,0x5F,0x22,0x1F,
    0x24,0x1F,0x24,0x5F,0x4A,0x40,0x58,0x4F,0x4E,0x75,0x00,0x00,0x00,0x00,0x00,0x00,
    0x14,0x01,0x4E,0xB0,0x25,0x91,0x22,0x5F,0x22,0x1F,0x24,0x1F,0x24,0x5F,0x4A,0x40,
    0x58,0x4F,0x4E,0x75
};

enum { ND_UNKNOWN = 0, ND_NONE, ND_CLASSIC, ND_LC2 };
static uint nd_pc24 = 0xFFFFFFFF;      /* last vector target seen (24-bit) */
static int  nd_kind = ND_UNKNOWN;
unsigned int mac_native_disp_count;     /* traps dispatched natively */

/* The LC II dispatcher is in ROM: compare it once through the same
 * instruction-fetch path the interpreter would use. */
extern unsigned int m68k_read_immediate_16(unsigned int address);

static int nd_identify(uint pc24)
{
    unsigned char *ram = mac_mem_get_ram();
    uint rs = mac_mem_get_ram_size();
    if (ram && pc24 < rs) {
        /* RAM: never cached, mac_native_aline() compares on every trap */
        return CPU_TYPE_IS_000(CPU_TYPE) && pc24 + sizeof(nd_classic_img) <= rs
               ? ND_CLASSIC : ND_NONE;
    }
    if (CPU_TYPE_IS_EC020_PLUS(CPU_TYPE) && pc24 >= 0x400000) {
        for (uint i = 0; i < sizeof(nd_lc2_img); i += 2) {
            uint w = m68k_read_immediate_16(pc24 + i) & 0xFFFF;
            if (w != (uint)((nd_lc2_img[i] << 8) | nd_lc2_img[i + 1]))
                return ND_NONE;
        }
        return ND_LC2;
    }
    return ND_NONE;
}

/* Cycle bookkeeping shared by both dispatchers.
 *   rem  = cycles left in the slice after the A-line instruction
 *          (what m68k_execute() tests before the next instruction)
 *   cyc  = cycles used by the natively executed instructions
 * STEP(off): the loop test before the instruction at base+off. */
#define CY(op)      (cyc += CYC_INSTRUCTION[(op)])
#define NOTAKE()    (cyc += CYC_BCC_NOTAKE_B)
#define STEP(off)   do { if (rem - cyc <= 0) { npc = base + (off); goto out; } \
                         REG_PPC = base + (off); } while (0)

/* Classic System 7 RAM dispatcher, 68000. Offsets are from the entry. */
static void nd_run_classic(uint base, sint rem)
{
    sint cyc = 0;
    uint npc, res, src, dst, ea;
    uint *D = REG_D, *A = REG_A;

    /* +00 MOVE.W D3,-(A7) */
    STEP(0x00);
    res = D[3] & 0xFFFF; A[7] -= 2; m68ki_write_16(A[7], res);
    FLAG_N = NFLAG_16(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x3F03);
    /* +02 MOVE.L A2,-(A7) */
    STEP(0x02);
    res = A[2]; A[7] -= 4;
    m68ki_write_16(A[7] + 2, res & 0xFFFF); m68ki_write_16(A[7], (res >> 16) & 0xFFFF);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x2F0A);
    /* +04 MOVEA.L 8(A7),A2 */
    STEP(0x04);
    A[2] = m68ki_read_32(A[7] + 8);
    CY(0x246F);
    /* +08 MOVE.W (A2)+,D3 */
    STEP(0x08);
    ea = A[2]; A[2] += 2; res = m68ki_read_16(ea);
    D[3] = MASK_OUT_BELOW_16(D[3]) | res;
    FLAG_N = NFLAG_16(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x361A);
    /* +0A CMPI.W #$A800,D3 */
    STEP(0x0A);
    src = 0xA800; dst = MASK_OUT_ABOVE_16(D[3]); res = dst - src;
    FLAG_N = NFLAG_16(res); FLAG_Z = MASK_OUT_ABOVE_16(res);
    FLAG_V = VFLAG_SUB_16(src, dst, res); FLAG_C = CFLAG_16(res);
    CY(0x0C43);
    /* +0E BCS.S +$7A (-> +8A, OS trap) */
    STEP(0x0E);
    CY(0x657A);
    if (COND_CS()) goto os_trap;
    NOTAKE();

    /* ---- Toolbox trap ---- */
    /* +10 ADD.W D3,D3 (x2) */
    STEP(0x10);
    src = MASK_OUT_ABOVE_16(D[3]); dst = src; res = src + dst;
    FLAG_N = NFLAG_16(res); FLAG_V = VFLAG_ADD_16(src, dst, res);
    FLAG_X = FLAG_C = CFLAG_16(res); FLAG_Z = MASK_OUT_ABOVE_16(res);
    D[3] = MASK_OUT_BELOW_16(D[3]) | FLAG_Z;
    CY(0xD643);
    STEP(0x12);
    src = MASK_OUT_ABOVE_16(D[3]); dst = src; res = src + dst;
    FLAG_N = NFLAG_16(res); FLAG_V = VFLAG_ADD_16(src, dst, res);
    FLAG_X = FLAG_C = CFLAG_16(res); FLAG_Z = MASK_OUT_ABOVE_16(res);
    D[3] = MASK_OUT_BELOW_16(D[3]) | FLAG_Z;
    CY(0xD643);
    /* +14 SUBI.W #$A800,D3 */
    STEP(0x14);
    src = 0xA800; dst = MASK_OUT_ABOVE_16(D[3]); res = dst - src;
    FLAG_N = NFLAG_16(res); FLAG_Z = MASK_OUT_ABOVE_16(res);
    FLAG_X = FLAG_C = CFLAG_16(res); FLAG_V = VFLAG_SUB_16(src, dst, res);
    D[3] = MASK_OUT_BELOW_16(D[3]) | FLAG_Z;
    CY(0x0443);
    /* +18 BCC.S +$14 (-> +2E) */
    STEP(0x18);
    CY(0x6414);
    if (COND_CC()) goto tb_2e;
    NOTAKE();
    /* +1A MOVE.L A2,8(A7) */
    STEP(0x1A);
    res = A[2]; m68ki_write_32(A[7] + 8, res);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x2F4A);
    /* +1E MOVEA.W D3,A2 */
    STEP(0x1E);
    A[2] = MAKE_INT_16(D[3]);
    CY(0x3443);
    /* +20 MOVE.W 4(A7),D3 */
    STEP(0x20);
    res = m68ki_read_16(A[7] + 4);
    D[3] = MASK_OUT_BELOW_16(D[3]) | res;
    FLAG_N = NFLAG_16(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x362F);
    /* +24 MOVE.L $1600(A2),4(A7) */
    STEP(0x24);
    res = m68ki_read_32(A[2] + 0x1600); m68ki_write_32(A[7] + 4, res);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x2F6A);
    /* +2A MOVEA.L (A7)+,A2 */
    STEP(0x2A);
    ea = A[7]; A[7] += 4; A[2] = m68ki_read_32(ea);
    CY(0x245F);
    goto rts_2c;

tb_2e:
    /* +2E ADDI.W #$0800,D3 */
    STEP(0x2E);
    src = 0x0800; dst = MASK_OUT_ABOVE_16(D[3]); res = src + dst;
    FLAG_N = NFLAG_16(res); FLAG_V = VFLAG_ADD_16(src, dst, res);
    FLAG_X = FLAG_C = CFLAG_16(res); FLAG_Z = MASK_OUT_ABOVE_16(res);
    D[3] = MASK_OUT_BELOW_16(D[3]) | FLAG_Z;
    CY(0x0643);
    /* +32 BCLR #12,D3 */
    STEP(0x32);
    FLAG_Z = D[3] & (1u << 12); D[3] &= ~(1u << 12);
    CY(0x0883);
    /* +36 BEQ.S +$18 (-> +50) */
    STEP(0x36);
    CY(0x6718);
    if (COND_EQ()) goto tb_50;
    NOTAKE();
    /* +38 BCLR #11,D3 */
    STEP(0x38);
    FLAG_Z = D[3] & (1u << 11); D[3] &= ~(1u << 11);
    CY(0x0883);
    /* +3C BEQ.S +$2C (-> +6A) */
    STEP(0x3C);
    CY(0x672C);
    if (COND_EQ()) goto tb_6a;
    NOTAKE();
    /* +3E MOVEA.W D3,A2 */
    STEP(0x3E);
    A[2] = MAKE_INT_16(D[3]);
    CY(0x3443);
    /* +40 ADDA.L ($0664).W,A2 */
    STEP(0x40);
    src = m68ki_read_32(MAKE_INT_16(0x0664)); A[2] = MASK_OUT_ABOVE_32(A[2] + src);
    CY(0xD5F8);
    /* +44 MOVE.L (A2),8(A7) */
    STEP(0x44);
    res = m68ki_read_32(A[2]); m68ki_write_32(A[7] + 8, res);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x2F52);
    /* +48 MOVEA.L (A7)+,A2 ; +4A MOVE.W (A7),D3 ; +4C ADDQ.W #4,A7 ; +4E RTS */
    STEP(0x48);
    ea = A[7]; A[7] += 4; A[2] = m68ki_read_32(ea);
    CY(0x245F);
    goto pop_4a;

tb_50:
    /* +50 BCLR #11,D3 */
    STEP(0x50);
    FLAG_Z = D[3] & (1u << 11); D[3] &= ~(1u << 11);
    CY(0x0883);
    /* +54 MOVE.L A2,8(A7) */
    STEP(0x54);
    res = A[2]; m68ki_write_32(A[7] + 8, res);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x2F4A);
    /* +58 MOVEA.W D3,A2 */
    STEP(0x58);
    A[2] = MAKE_INT_16(D[3]);
    CY(0x3443);
    /* +5A MOVE.W 4(A7),D3 */
    STEP(0x5A);
    res = m68ki_read_16(A[7] + 4);
    D[3] = MASK_OUT_BELOW_16(D[3]) | res;
    FLAG_N = NFLAG_16(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x362F);
    /* +5E ADDA.L ($0664).W,A2 */
    STEP(0x5E);
    src = m68ki_read_32(MAKE_INT_16(0x0664)); A[2] = MASK_OUT_ABOVE_32(A[2] + src);
    CY(0xD5F8);
    /* +62 MOVE.L (A2),4(A7) */
    STEP(0x62);
    res = m68ki_read_32(A[2]); m68ki_write_32(A[7] + 4, res);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x2F52);
    /* +66 MOVEA.L (A7)+,A2 */
    STEP(0x66);
    ea = A[7]; A[7] += 4; A[2] = m68ki_read_32(ea);
    CY(0x245F);
    /* +68 RTS */
    STEP(0x68);
    ea = A[7]; A[7] += 4; npc = m68ki_read_32(ea);
    CY(0x4E75);
    goto out;

tb_6a:
    /* +6A MOVEA.W D3,A2 */
    STEP(0x6A);
    A[2] = MAKE_INT_16(D[3]);
    CY(0x3443);
    /* +6C MOVE.L $0E00(A2),8(A7) */
    STEP(0x6C);
    res = m68ki_read_32(A[2] + 0x0E00); m68ki_write_32(A[7] + 8, res);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x2F6A);
    /* +72 MOVEA.L (A7)+,A2 */
    STEP(0x72);
    ea = A[7]; A[7] += 4; A[2] = m68ki_read_32(ea);
    CY(0x245F);
    /* +74 MOVE.W (A7),D3 ; +76 ADDQ.W #4,A7 ; +78 RTS */
    STEP(0x74);
    res = m68ki_read_16(A[7]);
    D[3] = MASK_OUT_BELOW_16(D[3]) | res;
    FLAG_N = NFLAG_16(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x3617);
    STEP(0x76);
    A[7] = MASK_OUT_ABOVE_32(A[7] + 4);
    CY(0x584F);
    STEP(0x78);
    ea = A[7]; A[7] += 4; npc = m68ki_read_32(ea);
    CY(0x4E75);
    goto out;

pop_4a:
    /* +4A MOVE.W (A7),D3 */
    STEP(0x4A);
    res = m68ki_read_16(A[7]);
    D[3] = MASK_OUT_BELOW_16(D[3]) | res;
    FLAG_N = NFLAG_16(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x3617);
    /* +4C ADDQ.W #4,A7 */
    STEP(0x4C);
    A[7] = MASK_OUT_ABOVE_32(A[7] + 4);
    CY(0x584F);
    /* +4E RTS */
    STEP(0x4E);
    ea = A[7]; A[7] += 4; npc = m68ki_read_32(ea);
    CY(0x4E75);
    goto out;

rts_2c:
    /* +2C RTS */
    STEP(0x2C);
    ea = A[7]; A[7] += 4; npc = m68ki_read_32(ea);
    CY(0x4E75);
    goto out;

os_trap:
    /* +8A MOVE.L A2,8(A7) */
    STEP(0x8A);
    res = A[2]; m68ki_write_32(A[7] + 8, res);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x2F4A);
    /* +8E MOVE.L D2,-(A7) ; +90 MOVE.L D1,-(A7) ; +92 MOVE.L A1,-(A7) */
    STEP(0x8E);
    res = D[2]; A[7] -= 4;
    m68ki_write_16(A[7] + 2, res & 0xFFFF); m68ki_write_16(A[7], (res >> 16) & 0xFFFF);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x2F02);
    STEP(0x90);
    res = D[1]; A[7] -= 4;
    m68ki_write_16(A[7] + 2, res & 0xFFFF); m68ki_write_16(A[7], (res >> 16) & 0xFFFF);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x2F01);
    STEP(0x92);
    res = A[1]; A[7] -= 4;
    m68ki_write_16(A[7] + 2, res & 0xFFFF); m68ki_write_16(A[7], (res >> 16) & 0xFFFF);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x2F09);
    /* +94 MOVE.W D3,D1 */
    STEP(0x94);
    res = MASK_OUT_ABOVE_16(D[3]); D[1] = MASK_OUT_BELOW_16(D[1]) | res;
    FLAG_N = NFLAG_16(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x3203);
    /* +96 ANDI.W #$0100,D3 */
    STEP(0x96);
    FLAG_Z = MASK_OUT_ABOVE_16(D[3] &= (0x0100 | 0xffff0000));
    FLAG_N = NFLAG_16(FLAG_Z); FLAG_C = CFLAG_CLEAR; FLAG_V = VFLAG_CLEAR;
    CY(0x0243);
    /* +9A BNE.S +$22 (-> +BE) */
    STEP(0x9A);
    CY(0x6622);
    if (COND_NE()) goto os_be;
    NOTAKE();
    /* +9C MOVE.L A0,-(A7) */
    STEP(0x9C);
    res = A[0]; A[7] -= 4;
    m68ki_write_16(A[7] + 2, res & 0xFFFF); m68ki_write_16(A[7], (res >> 16) & 0xFFFF);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x2F08);
    /* +9E MOVE.B D1,D3 */
    STEP(0x9E);
    res = MASK_OUT_ABOVE_8(D[1]); D[3] = MASK_OUT_BELOW_8(D[3]) | res;
    FLAG_N = NFLAG_8(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x1601);
    /* +A0 ADD.W D3,D3 (x2) */
    STEP(0xA0);
    src = MASK_OUT_ABOVE_16(D[3]); dst = src; res = src + dst;
    FLAG_N = NFLAG_16(res); FLAG_V = VFLAG_ADD_16(src, dst, res);
    FLAG_X = FLAG_C = CFLAG_16(res); FLAG_Z = MASK_OUT_ABOVE_16(res);
    D[3] = MASK_OUT_BELOW_16(D[3]) | FLAG_Z;
    CY(0xD643);
    STEP(0xA2);
    src = MASK_OUT_ABOVE_16(D[3]); dst = src; res = src + dst;
    FLAG_N = NFLAG_16(res); FLAG_V = VFLAG_ADD_16(src, dst, res);
    FLAG_X = FLAG_C = CFLAG_16(res); FLAG_Z = MASK_OUT_ABOVE_16(res);
    D[3] = MASK_OUT_BELOW_16(D[3]) | FLAG_Z;
    CY(0xD643);
    /* +A4 MOVEA.W D3,A2 */
    STEP(0xA4);
    A[2] = MAKE_INT_16(D[3]);
    CY(0x3443);
    /* +A6 MOVE.W $14(A7),D3 */
    STEP(0xA6);
    res = m68ki_read_16(A[7] + 0x14);
    D[3] = MASK_OUT_BELOW_16(D[3]) | res;
    FLAG_N = NFLAG_16(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x362F);
    /* +AA MOVEA.L $0400(A2),A2 */
    STEP(0xAA);
    A[2] = m68ki_read_32(A[2] + 0x0400);
    CY(0x246A);
    /* +AE JSR (A2) */
    STEP(0xAE);
    ea = A[2];
    A[7] -= 4; m68ki_write_32(A[7], base + 0xB0);
    npc = ea;
    CY(0x4E92);
    goto out;

os_be:
    /* +BE MOVE.B D1,D3 */
    STEP(0xBE);
    res = MASK_OUT_ABOVE_8(D[1]); D[3] = MASK_OUT_BELOW_8(D[3]) | res;
    FLAG_N = NFLAG_8(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x1601);
    /* +C0 ADD.W D3,D3 (x2) */
    STEP(0xC0);
    src = MASK_OUT_ABOVE_16(D[3]); dst = src; res = src + dst;
    FLAG_N = NFLAG_16(res); FLAG_V = VFLAG_ADD_16(src, dst, res);
    FLAG_X = FLAG_C = CFLAG_16(res); FLAG_Z = MASK_OUT_ABOVE_16(res);
    D[3] = MASK_OUT_BELOW_16(D[3]) | FLAG_Z;
    CY(0xD643);
    STEP(0xC2);
    src = MASK_OUT_ABOVE_16(D[3]); dst = src; res = src + dst;
    FLAG_N = NFLAG_16(res); FLAG_V = VFLAG_ADD_16(src, dst, res);
    FLAG_X = FLAG_C = CFLAG_16(res); FLAG_Z = MASK_OUT_ABOVE_16(res);
    D[3] = MASK_OUT_BELOW_16(D[3]) | FLAG_Z;
    CY(0xD643);
    /* +C4 MOVEA.W D3,A2 */
    STEP(0xC4);
    A[2] = MAKE_INT_16(D[3]);
    CY(0x3443);
    /* +C6 MOVE.W $10(A7),D3 */
    STEP(0xC6);
    res = m68ki_read_16(A[7] + 0x10);
    D[3] = MASK_OUT_BELOW_16(D[3]) | res;
    FLAG_N = NFLAG_16(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x362F);
    /* +CA MOVEA.L (A2),A2 */
    STEP(0xCA);
    A[2] = m68ki_read_32(A[2]);
    CY(0x2452);
    /* +CC JSR (A2) */
    STEP(0xCC);
    ea = A[2];
    A[7] -= 4; m68ki_write_32(A[7], base + 0xCE);
    npc = ea;
    CY(0x4E92);
    goto out;

out:
    REG_PC = npc;
    USE_CYCLES(cyc);
    if (cyc) mac_native_disp_count++;
}

/* 68020+ full-format index EA (BS=1, index Dn.w*scale, word BD, optional
 * null-outer memory indirect pre-index), as in m68ki_get_ea_ix(). */
static inline uint nd_ea_fullix(uint ext, uint bd, sint *cyc)
{
    *cyc += m68ki_ea_idx_cycle_table[ext & 0x3f];
    uint Xn = REG_DA[ext >> 12];
    if (!BIT_B(ext)) Xn = MAKE_INT_16(Xn);
    Xn <<= (ext >> 9) & 3;
    if (!(ext & 7))
        return bd + Xn;
    return m68ki_read_32(bd + Xn);      /* pre-index, null outer */
}

/* LC II ROM dispatcher, 68020+ frame (SR, PC, format word). */
static void nd_run_lc2(uint base, sint rem)
{
    sint cyc = 0;
    uint npc, res, src, dst, ea;
    uint *D = REG_D, *A = REG_A;

    /* +00 MOVE.L A2,-(A7) */
    STEP(0x00);
    res = A[2]; A[7] -= 4;
    m68ki_write_16(A[7] + 2, res & 0xFFFF); m68ki_write_16(A[7], (res >> 16) & 0xFFFF);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x2F0A);
    /* +02 MOVE.L D2,-(A7) */
    STEP(0x02);
    res = D[2]; A[7] -= 4;
    m68ki_write_16(A[7] + 2, res & 0xFFFF); m68ki_write_16(A[7], (res >> 16) & 0xFFFF);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x2F02);
    /* +04 MOVEA.L $A(A7),A2 */
    STEP(0x04);
    A[2] = m68ki_read_32(A[7] + 0x0A);
    CY(0x246F);
    /* +08 MOVE.W (A2)+,D2 */
    STEP(0x08);
    ea = A[2]; A[2] += 2; res = m68ki_read_16(ea);
    D[2] = MASK_OUT_BELOW_16(D[2]) | res;
    FLAG_N = NFLAG_16(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x341A);
    /* +0A CMPI.W #$A800,D2 */
    STEP(0x0A);
    src = 0xA800; dst = MASK_OUT_ABOVE_16(D[2]); res = dst - src;
    FLAG_N = NFLAG_16(res); FLAG_Z = MASK_OUT_ABOVE_16(res);
    FLAG_V = VFLAG_SUB_16(src, dst, res); FLAG_C = CFLAG_16(res);
    CY(0x0C42);
    /* +0E BCS.S +$30 (-> +40) */
    STEP(0x0E);
    CY(0x6530);
    if (COND_CS()) goto os_trap;
    NOTAKE();
    /* +10 SUBI.W #$AC00,D2 */
    STEP(0x10);
    src = 0xAC00; dst = MASK_OUT_ABOVE_16(D[2]); res = dst - src;
    FLAG_N = NFLAG_16(res); FLAG_Z = MASK_OUT_ABOVE_16(res);
    FLAG_X = FLAG_C = CFLAG_16(res); FLAG_V = VFLAG_SUB_16(src, dst, res);
    D[2] = MASK_OUT_BELOW_16(D[2]) | FLAG_Z;
    CY(0x0442);
    /* +14 BCC.S +$1A (-> +30, autopop) */
    STEP(0x14);
    CY(0x641A);
    if (COND_CC()) goto autopop;
    NOTAKE();
    /* +16 MOVE.L ($1E00,D2.w*4),8(A7) */
    STEP(0x16);
    CY(0x2F70);
    res = m68ki_read_32(nd_ea_fullix(0x25A0, 0x1E00, &cyc));
    m68ki_write_32(A[7] + 8, res);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    /* +1E MOVE.L A2,$C(A7) */
    STEP(0x1E);
    res = A[2]; m68ki_write_32(A[7] + 0x0C, res);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x2F4A);
    /* +22 MOVE.L (A7)+,D2 */
    STEP(0x22);
    ea = A[7]; A[7] += 4; res = m68ki_read_32(ea); D[2] = res;
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x241F);
    /* +24 MOVEA.L (A7)+,A2 */
    STEP(0x24);
    ea = A[7]; A[7] += 4; A[2] = m68ki_read_32(ea);
    CY(0x245F);
    /* +26 RTS */
    STEP(0x26);
    ea = A[7]; A[7] += 4; npc = m68ki_read_32(ea);
    CY(0x4E75);
    goto out;

autopop:
    /* +30 MOVE.L ($0E00,D2.w*4),8(A7) */
    STEP(0x30);
    CY(0x2F70);
    res = m68ki_read_32(nd_ea_fullix(0x25A0, 0x0E00, &cyc));
    m68ki_write_32(A[7] + 8, res);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    /* +38 MOVE.L (A7)+,D2 */
    STEP(0x38);
    ea = A[7]; A[7] += 4; res = m68ki_read_32(ea); D[2] = res;
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x241F);
    /* +3A MOVEA.L (A7)+,A2 */
    STEP(0x3A);
    ea = A[7]; A[7] += 4; A[2] = m68ki_read_32(ea);
    CY(0x245F);
    /* +3C RTD #4 */
    STEP(0x3C);
    ea = A[7]; A[7] += 4; npc = m68ki_read_32(ea);
    A[7] = MASK_OUT_ABOVE_32(A[7] + MAKE_INT_16(0x0004));
    CY(0x4E74);
    goto out;

os_trap:
    /* +40 MOVE.L D1,-(A7) ; +42 MOVE.L A1,-(A7) */
    STEP(0x40);
    res = D[1]; A[7] -= 4;
    m68ki_write_16(A[7] + 2, res & 0xFFFF); m68ki_write_16(A[7], (res >> 16) & 0xFFFF);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x2F01);
    STEP(0x42);
    res = A[1]; A[7] -= 4;
    m68ki_write_16(A[7] + 2, res & 0xFFFF); m68ki_write_16(A[7], (res >> 16) & 0xFFFF);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x2F09);
    /* +44 MOVE.W D2,D1 */
    STEP(0x44);
    res = MASK_OUT_ABOVE_16(D[2]); D[1] = MASK_OUT_BELOW_16(D[1]) | res;
    FLAG_N = NFLAG_16(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x3202);
    /* +46 MOVE.L A2,$14(A7) */
    STEP(0x46);
    res = A[2]; m68ki_write_32(A[7] + 0x14, res);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x2F4A);
    /* +4A ANDI.W #$0100,D2 */
    STEP(0x4A);
    FLAG_Z = MASK_OUT_ABOVE_16(D[2] &= (0x0100 | 0xffff0000));
    FLAG_N = NFLAG_16(FLAG_Z); FLAG_C = CFLAG_CLEAR; FLAG_V = VFLAG_CLEAR;
    CY(0x0242);
    /* +4E BNE.S +$20 (-> +70) */
    STEP(0x4E);
    CY(0x6620);
    if (COND_NE()) goto os_70;
    NOTAKE();
    /* +50 MOVE.B D1,D2 */
    STEP(0x50);
    res = MASK_OUT_ABOVE_8(D[1]); D[2] = MASK_OUT_BELOW_8(D[2]) | res;
    FLAG_N = NFLAG_8(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x1401);
    /* +52 MOVE.L A0,-(A7) */
    STEP(0x52);
    res = A[0]; A[7] -= 4;
    m68ki_write_16(A[7] + 2, res & 0xFFFF); m68ki_write_16(A[7], (res >> 16) & 0xFFFF);
    FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x2F08);
    /* +54 JSR ([$0400,D2.w*4]) */
    STEP(0x54);
    CY(0x4EB0);
    ea = nd_ea_fullix(0x25A1, 0x0400, &cyc);
    A[7] -= 4; m68ki_write_32(A[7], base + 0x5A);
    npc = ea;
    goto out;

os_70:
    /* +70 MOVE.B D1,D2 */
    STEP(0x70);
    res = MASK_OUT_ABOVE_8(D[1]); D[2] = MASK_OUT_BELOW_8(D[2]) | res;
    FLAG_N = NFLAG_8(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
    CY(0x1401);
    /* +72 JSR ([D2.w*4]) */
    STEP(0x72);
    CY(0x4EB0);
    ea = nd_ea_fullix(0x2591, 0, &cyc);
    A[7] -= 4; m68ki_write_32(A[7], base + 0x76);
    npc = ea;
    goto out;

out:
    REG_PC = npc;
    USE_CYCLES(cyc);
    if (cyc) mac_native_disp_count++;
}

/* Called at the end of m68ki_exception_1010(): the frame is built and
 * REG_PC is the A-line vector's target. */
void mac_native_aline(void)
{
    uint pc = REG_PC;
    uint pc24 = pc & 0xFFFFFF;
    if (pc24 != nd_pc24 || nd_kind == ND_UNKNOWN) {
        nd_pc24 = pc24;
        nd_kind = nd_identify(pc24);
    }
    /* What m68k_execute() tests before the next instruction: the loop
     * subtracts CYC_INSTRUCTION[REG_IR] after this exception returns. */
    sint rem = GET_CYCLES() - (sint)CYC_INSTRUCTION[REG_IR];
    if (nd_kind == ND_CLASSIC) {
        /* RAM code: compare on every trap (it may be patched or replaced;
         * a mismatch usually fails on the first bytes) */
        if (memcmp(mac_mem_get_ram() + pc24, nd_classic_img, sizeof(nd_classic_img)) == 0)
            nd_run_classic(pc, rem);
    } else if (nd_kind == ND_LC2) {
        nd_run_lc2(pc, rem);
    }
}

#endif /* MAC_NATIVE_TRAPDISP */

/* ======================================================================== */
/* 2. MAC_NATIVE_DCMP: resource decompressor 'dcmp' 3 (System 7.5.x).       */
/*                                                                          */
/* Nearly every compressed resource of System 7.5.3 (both boot disks: 529   */
/* and 614 resources, header version 9) names 'dcmp' ID 3, a 7696-byte      */
/* C-compiled LZ77 decoder with prefix-coded lengths and position-dependent */
/* offset codes. It is 33% (LC II) to 65% (Classic) of all boot             */
/* instructions. The same code is loaded from the System file on both       */
/* machines (Classic $8CE8, LC II $871C) and called by the System's         */
/* decompression glue in three steps: entry 0 (+$06, init), entry 1 (+$20,  */
/* decompress(hdr, dst, src)), entry 2 (+$12, done).                        */
/*                                                                          */
/* Hook: when the glue calls Get1Resource('dcmp', 3) (A-line pre-hook,      */
/* exception path only), the resource map of CurMap is walked; if the dcmp  */
/* is loaded and its 7696 bytes hash to the known code, the first word of   */
/* entry 1 (LINK A6,#-$2A) is replaced by the trap word DC_TRAP. Executing  */
/* it lands in dc_trap(), which re-checks the hash (with the original word),*/
/* decompresses in C and returns like entry 1 does. If the hash does not    */
/* match (code moved/changed), the original word is put back and the        */
/* instruction re-executed by the interpreter.                              */
/*                                                                          */
/* The C version reproduces everything the 68k routine leaves behind above  */
/* the stack pointer: output bytes (same byte order, overlapping matches,   */
/* reads before dst), the CLR.W of the word at entry SP+$10, D0/D1/D2 (the  */
/* scratch registers it changes; D3-D7/A1-A6 are preserved by it), A0 (=    */
/* return address), A7 and the condition codes. Not reproduced: the dead    */
/* stack bytes below the caller's SP and the CPU time (it runs in zero      */
/* emulated cycles), so runs with it ON are not cycle-identical to OFF.     */
/* MAC_NATIVE_DCMP_CHECK=1 (host) additionally runs the original 68k code   */
/* on the same input (nested, interrupts held off) and compares RAM (except */
/* the dead stack window), registers and flags after every call.            */
/* ======================================================================== */

#if MAC_NATIVE_DCMP

#define DC_LEN      0x1E10u             /* code bytes of 'dcmp' 3 */
#define DC_HASH     0x394D8203u         /* FNV-1a of those bytes */
#define DC_ENTRY1   0x20u
#define DC_OP1      0x4E56u             /* LINK A6,#-$2A at entry 1 */
/* DC_TRAP (m68kconf.h): the A-line word patched over DC_OP1. */

unsigned int mac_native_dcmp_count, mac_native_dcmp_bytes, mac_native_dcmp_patches;
#if MAC_NATIVE_DCMP_CHECK
unsigned int mac_native_dcmp_bad;
#endif

static uint8_t *dc_ram;
static uint32_t dc_ram_size;

static inline uint32_t dc_rd8(uint32_t a)
{
    a &= 0xFFFFFF;
    if (a < dc_ram_size) return dc_ram[a];
    return m68k_read_memory_8(a);
}
static inline void dc_wr8(uint32_t a, uint32_t v)
{
    a &= 0xFFFFFF;
    if (a >= 0x0A20 && a < dc_ram_size) { dc_ram[a] = (uint8_t)v; return; }
    m68k_write_memory_8(a, v & 0xFF);
}
static inline uint32_t dc_rd16(uint32_t a) { return (dc_rd8(a) << 8) | dc_rd8(a + 1); }
static inline uint32_t dc_rd32(uint32_t a) { return (dc_rd16(a) << 16) | dc_rd16(a + 2); }

/* 68k LSR.L Dn,Dm: count is taken mod 64; 32..63 give 0 */
static inline uint32_t dc_lsr(uint32_t v, uint32_t c)
{
    c &= 63;
    return c >= 32 ? 0 : v >> c;
}

static uint32_t dc_hash(uint32_t base, uint32_t patched_off, uint32_t orig)
{
    uint32_t h = 0x811C9DC5u;
    for (uint32_t i = 0; i < DC_LEN; i++) {
        uint32_t b = dc_ram[base + i];
        if (i == patched_off) b = orig >> 8;
        else if (i == patched_off + 1) b = orig & 0xFF;
        h = (h ^ b) * 0x01000193u;
    }
    return h;
}

/* Bit reader state (the routine's struct at -$26(A6) of entry 1) and the
 * scratch registers it leaves behind. */
typedef struct {
    uint32_t ptr, buf, bytes, cnt, total;
    uint32_t d1, d2;
} dcs_t;

/* $AA2A getbits(n), n <= 8 (one refill byte) */
static uint32_t dc_gb(dcs_t *s, uint32_t n)
{
    int32_t d6 = (int32_t)(s->cnt - n);
    uint32_t d7 = s->buf;
    if (d6 < 0) {
        s->bytes++;
        d7 = (d7 << 8) | dc_rd8(s->ptr);
        s->ptr++;
        d6 += 8;
        s->buf = d7;
    }
    s->cnt = (uint32_t)d6;
    uint32_t d0 = dc_lsr(d7, (uint32_t)d6);
    uint32_t d1 = (32 - n) & 0xFF;          /* MOVEQ #$20,D1 ; SUB.B D5,D1 */
    uint32_t d2 = dc_lsr(0xFFFFFFFFu, d1) & d0;
    s->d1 = d1; s->d2 = d2;
    return d2;
}

/* $A9BE getbits(n) for 9..20 bits (refills to 24 bits) */
static uint32_t dc_gbl(dcs_t *s, uint32_t n)
{
    while ((int32_t)s->cnt < 23) {          /* MOVEQ #23,D0 ; CMP.L 12(A4),D0 ; BGT */
        s->buf = (s->buf << 8) | dc_rd8(s->ptr);
        s->ptr++;
        s->bytes++;
        s->cnt += 8;
    }
    s->total += n;
    s->cnt -= n;
    uint32_t d1 = dc_lsr(s->buf, s->cnt);
    uint32_t d0 = (32 - n) & 0xFF;          /* MOVEQ #$20,D0 ; SUB.B D7,D0 */
    uint32_t d2 = dc_lsr(0xFFFFFFFFu, d0);
    s->d1 = d1; s->d2 = d2;
    return d2 & d1;
}

/* $AA80 copy n literal bytes from the bit stream (keeps the bit phase) */
static uint32_t dc_copybytes(dcs_t *s, uint32_t dst, int32_t n)
{
    uint32_t d6 = s->cnt, d7 = s->buf;
    for (;;) {
        if ((int32_t)8 > (int32_t)d6) break;
        int32_t old = n--;
        if (old == 0) break;
        d6 -= 8;
        dc_wr8(dst++, dc_lsr(d7, d6));
    }
    if (n > 0) {
        uint32_t p = s->ptr;
        s->bytes += (uint32_t)n;
        do {
            d7 = (d7 << 8) | dc_rd8(p);
            p++;
            s->d1 = d7;
            dc_wr8(dst++, dc_lsr(d7, d6));
        } while (--n != 0);
        s->ptr = p;
        s->buf = d7;
    }
    s->cnt = d6;
    return dst;
}

/* $8F7C match length code (0 = literal run / implicit length 3) */
static uint32_t dc_len(dcs_t *s)
{
    int d7 = 0;
    while (d7 < 10) {
        if (dc_gb(s, 1) == 0) break;
        d7++;
    }
    switch (d7) {
    case 0:  return dc_gb(s, 1);
    case 1:  if (dc_gb(s, 1) == 0) return 2;
             return dc_gb(s, 1) + 3;
    case 2:  if (dc_gb(s, 1) == 0) return dc_gb(s, 1) + 5;
             return dc_gb(s, 2) + 7;
    case 3:  { uint32_t v = dc_gb(s, 3); s->d1 = 11;  return v + 11; }
    case 4:  { uint32_t v = dc_gb(s, 3); s->d1 = 19;  return v + 19; }
    case 5:  { uint32_t v = dc_gb(s, 5); s->d1 = 27;  return v + 27; }
    case 6:  { uint32_t v = dc_gb(s, 6); s->d1 = 59;  return v + 59; }
    case 7:  { uint32_t v = dc_gb(s, 7); s->d1 = 123; return v + 123; }
    case 8:  return dc_gb(s, 8) + 0xFB;
    case 9:  return dc_gbl(s, 9) + 0x1FB;
    default: return dc_gbl(s, 10) + 0x3FB;
    }
}

/* $8EA0 literal run length (1..63) */
static uint32_t dc_litcount(dcs_t *s)
{
    if (dc_gb(s, 1) == 0) return 1;
    switch (dc_gb(s, 2)) {
    case 0:  return 2;
    case 1:  return 3;
    case 2:  return dc_gb(s, 2) + 4;
    default: {
        int16_t d7 = (int16_t)dc_gb(s, 4);
        if (d7 <= 7) return (uint32_t)(int32_t)d7 + 8;
        if (d7 <= 11) {
            uint32_t base = (((uint32_t)(int32_t)d7 - 8) << 2) + 16;
            uint32_t v = dc_gb(s, 2);
            s->d1 = base;
            return v + base;
        }
        uint32_t base = (((uint32_t)(int32_t)d7 - 12) << 3) + 32;
        uint32_t v = dc_gb(s, 3);
        s->d1 = base;
        return v + base;
    }
    }
}
/* Generated by scratchpad gen/genoff.py from the dcmp-3 disassembly. */
static uint32_t dc_off1(dcs_t *s, int32_t pos)
{
    uint32_t d0;
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L0580;
    d0 = 1;
    goto out;
L0580:
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L05A0;
    d0 = 2;
    d0 = dc_gb(s, d0);
    d0 += 2;
    goto out;
L05A0:
    d0 = 7;
    if ((int32_t)d0 < pos) goto L05B6;
    d0 = 1;
    d0 = dc_gb(s, d0);
    d0 += 6;
    goto out;
L05B6:
    d0 = 9;
    if ((int32_t)d0 < pos) goto L05CC;
    d0 = 2;
    d0 = dc_gb(s, d0);
    d0 += 6;
    goto out;
L05CC:
    d0 = 13;
    if ((int32_t)d0 < pos) goto L05E2;
    d0 = 3;
    d0 = dc_gb(s, d0);
    d0 += 6;
    goto out;
L05E2:
    d0 = 4;
    d0 = dc_gb(s, d0);
    d0 += 6;
out:
    return d0;
}

static uint32_t dc_off2(dcs_t *s, int32_t pos)
{
    uint32_t d0;
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L062C;
    d0 = 1;
    d0 = dc_gb(s, d0);
    d0 += 1;
    goto out;
L062C:
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L064C;
    d0 = 3;
    d0 = dc_gb(s, d0);
    d0 += 3;
    goto out;
L064C:
    d0 = 12;
    if ((int32_t)d0 < pos) goto L0664;
    d0 = 1;
    d0 = dc_gb(s, d0);
    s->d1 = 11;
    d0 += s->d1;
    goto out;
L0664:
    d0 = 14;
    if ((int32_t)d0 < pos) goto L067C;
    d0 = 2;
    d0 = dc_gb(s, d0);
    s->d1 = 11;
    d0 += s->d1;
    goto out;
L067C:
    d0 = 18;
    if ((int32_t)d0 < pos) goto L0694;
    d0 = 3;
    d0 = dc_gb(s, d0);
    s->d1 = 11;
    d0 += s->d1;
    goto out;
L0694:
    d0 = 26;
    if ((int32_t)d0 < pos) goto L06AC;
    d0 = 4;
    d0 = dc_gb(s, d0);
    s->d1 = 11;
    d0 += s->d1;
    goto out;
L06AC:
    d0 = 5;
    d0 = dc_gb(s, d0);
    s->d1 = 11;
    d0 += s->d1;
out:
    return d0;
}

static uint32_t dc_off3(dcs_t *s, int32_t pos)
{
    uint32_t d0;
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L06F8;
    d0 = 2;
    d0 = dc_gb(s, d0);
    d0 += 1;
    goto out;
L06F8:
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L071A;
    d0 = 4;
    d0 = dc_gb(s, d0);
    d0 += 5;
    goto out;
L071A:
    d0 = 22;
    if ((int32_t)d0 < pos) goto L0732;
    d0 = 1;
    d0 = dc_gb(s, d0);
    s->d1 = 21;
    d0 += s->d1;
    goto out;
L0732:
    d0 = 24;
    if ((int32_t)d0 < pos) goto L074A;
    d0 = 2;
    d0 = dc_gb(s, d0);
    s->d1 = 21;
    d0 += s->d1;
    goto out;
L074A:
    d0 = 28;
    if ((int32_t)d0 < pos) goto L0762;
    d0 = 3;
    d0 = dc_gb(s, d0);
    s->d1 = 21;
    d0 += s->d1;
    goto out;
L0762:
    d0 = 36;
    if ((int32_t)d0 < pos) goto L077A;
    d0 = 4;
    d0 = dc_gb(s, d0);
    s->d1 = 21;
    d0 += s->d1;
    goto out;
L077A:
    d0 = 52;
    if ((int32_t)d0 < pos) goto L0792;
    d0 = 5;
    d0 = dc_gb(s, d0);
    s->d1 = 21;
    d0 += s->d1;
    goto out;
L0792:
    d0 = 6;
    d0 = dc_gb(s, d0);
    s->d1 = 21;
    d0 += s->d1;
out:
    return d0;
}

static uint32_t dc_off4(dcs_t *s, int32_t pos)
{
    uint32_t d0;
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L07DE;
    d0 = 3;
    d0 = dc_gb(s, d0);
    d0 += 1;
    goto out;
L07DE:
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L0802;
    d0 = 5;
    d0 = dc_gb(s, d0);
    s->d1 = 9;
    d0 += s->d1;
    goto out;
L0802:
    d0 = 42;
    if ((int32_t)d0 < pos) goto L081C;
    d0 = 1;
    d0 = dc_gb(s, d0);
    s->d1 = 41;
    d0 += s->d1;
    goto out;
L081C:
    d0 = 44;
    if ((int32_t)d0 < pos) goto L0834;
    d0 = 2;
    d0 = dc_gb(s, d0);
    s->d1 = 41;
    d0 += s->d1;
    goto out;
L0834:
    d0 = 48;
    if ((int32_t)d0 < pos) goto L084C;
    d0 = 3;
    d0 = dc_gb(s, d0);
    s->d1 = 41;
    d0 += s->d1;
    goto out;
L084C:
    d0 = 56;
    if ((int32_t)d0 < pos) goto L0864;
    d0 = 4;
    d0 = dc_gb(s, d0);
    s->d1 = 41;
    d0 += s->d1;
    goto out;
L0864:
    d0 = 72;
    if ((int32_t)d0 < pos) goto L087C;
    d0 = 5;
    d0 = dc_gb(s, d0);
    s->d1 = 41;
    d0 += s->d1;
    goto out;
L087C:
    d0 = 104;
    if ((int32_t)d0 < pos) goto L0894;
    d0 = 6;
    d0 = dc_gb(s, d0);
    s->d1 = 41;
    d0 += s->d1;
    goto out;
L0894:
    d0 = 7;
    d0 = dc_gb(s, d0);
    s->d1 = 41;
    d0 += s->d1;
out:
    return d0;
}

static uint32_t dc_off5(dcs_t *s, int32_t pos)
{
    uint32_t d0;
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L08E0;
    d0 = 4;
    d0 = dc_gb(s, d0);
    d0 += 1;
    goto out;
L08E0:
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L0904;
    d0 = 6;
    d0 = dc_gb(s, d0);
    s->d1 = 17;
    d0 += s->d1;
    goto out;
L0904:
    d0 = 82;
    if ((int32_t)d0 < pos) goto L091E;
    d0 = 1;
    d0 = dc_gb(s, d0);
    s->d1 = 81;
    d0 += s->d1;
    goto out;
L091E:
    d0 = 84;
    if ((int32_t)d0 < pos) goto L0938;
    d0 = 2;
    d0 = dc_gb(s, d0);
    s->d1 = 81;
    d0 += s->d1;
    goto out;
L0938:
    d0 = 88;
    if ((int32_t)d0 < pos) goto L0950;
    d0 = 3;
    d0 = dc_gb(s, d0);
    s->d1 = 81;
    d0 += s->d1;
    goto out;
L0950:
    d0 = 96;
    if ((int32_t)d0 < pos) goto L0968;
    d0 = 4;
    d0 = dc_gb(s, d0);
    s->d1 = 81;
    d0 += s->d1;
    goto out;
L0968:
    d0 = 112;
    if ((int32_t)d0 < pos) goto L0980;
    d0 = 5;
    d0 = dc_gb(s, d0);
    s->d1 = 81;
    d0 += s->d1;
    goto out;
L0980:
    if (pos > 144) goto L099A;
    d0 = 6;
    d0 = dc_gb(s, d0);
    s->d1 = 81;
    d0 += s->d1;
    goto out;
L099A:
    if (pos > 208) goto L09B4;
    d0 = 7;
    d0 = dc_gb(s, d0);
    s->d1 = 81;
    d0 += s->d1;
    goto out;
L09B4:
    d0 = 8;
    d0 = dc_gb(s, d0);
    s->d1 = 81;
    d0 += s->d1;
out:
    return d0;
}

static uint32_t dc_off6(dcs_t *s, int32_t pos)
{
    uint32_t d0;
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L0A00;
    d0 = 5;
    d0 = dc_gb(s, d0);
    d0 += 1;
    goto out;
L0A00:
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L0A24;
    d0 = 7;
    d0 = dc_gb(s, d0);
    s->d1 = 33;
    d0 += s->d1;
    goto out;
L0A24:
    if (pos > 162) goto L0A42;
    d0 = 1;
    d0 = dc_gb(s, d0);
    d0 += 0xA1;
    goto out;
L0A42:
    if (pos > 164) goto L0A60;
    d0 = 2;
    d0 = dc_gb(s, d0);
    d0 += 0xA1;
    goto out;
L0A60:
    if (pos > 168) goto L0A7E;
    d0 = 3;
    d0 = dc_gb(s, d0);
    d0 += 0xA1;
    goto out;
L0A7E:
    if (pos > 176) goto L0A9C;
    d0 = 4;
    d0 = dc_gb(s, d0);
    d0 += 0xA1;
    goto out;
L0A9C:
    if (pos > 192) goto L0AB8;
    d0 = 5;
    d0 = dc_gb(s, d0);
    d0 += 0xA1;
    goto out;
L0AB8:
    if (pos > 224) goto L0AD4;
    d0 = 6;
    d0 = dc_gb(s, d0);
    d0 += 0xA1;
    goto out;
L0AD4:
    if (pos > 288) goto L0AF0;
    d0 = 7;
    d0 = dc_gb(s, d0);
    d0 += 0xA1;
    goto out;
L0AF0:
    if (pos > 416) goto L0B0C;
    d0 = 8;
    d0 = dc_gb(s, d0);
    d0 += 0xA1;
    goto out;
L0B0C:
    d0 = 9;
    d0 = dc_gbl(s, d0);
    d0 += 0xA1;
out:
    return d0;
}

static uint32_t dc_off7(dcs_t *s, int32_t pos)
{
    uint32_t d0;
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L0B5A;
    d0 = 6;
    d0 = dc_gb(s, d0);
    d0 += 1;
    goto out;
L0B5A:
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L0B7E;
    d0 = 8;
    d0 = dc_gb(s, d0);
    s->d1 = 65;
    d0 += s->d1;
    goto out;
L0B7E:
    if (pos > 322) goto L0B9C;
    d0 = 1;
    d0 = dc_gb(s, d0);
    d0 += 0x141;
    goto out;
L0B9C:
    if (pos > 324) goto L0BBA;
    d0 = 2;
    d0 = dc_gb(s, d0);
    d0 += 0x141;
    goto out;
L0BBA:
    if (pos > 328) goto L0BD8;
    d0 = 3;
    d0 = dc_gb(s, d0);
    d0 += 0x141;
    goto out;
L0BD8:
    if (pos > 336) goto L0BF6;
    d0 = 4;
    d0 = dc_gb(s, d0);
    d0 += 0x141;
    goto out;
L0BF6:
    if (pos > 352) goto L0C14;
    d0 = 5;
    d0 = dc_gb(s, d0);
    d0 += 0x141;
    goto out;
L0C14:
    if (pos > 384) goto L0C30;
    d0 = 6;
    d0 = dc_gb(s, d0);
    d0 += 0x141;
    goto out;
L0C30:
    if (pos > 448) goto L0C4C;
    d0 = 7;
    d0 = dc_gb(s, d0);
    d0 += 0x141;
    goto out;
L0C4C:
    if (pos > 576) goto L0C68;
    d0 = 8;
    d0 = dc_gb(s, d0);
    d0 += 0x141;
    goto out;
L0C68:
    if (pos > 832) goto L0C84;
    d0 = 9;
    d0 = dc_gbl(s, d0);
    d0 += 0x141;
    goto out;
L0C84:
    d0 = 10;
    d0 = dc_gbl(s, d0);
    d0 += 0x141;
out:
    return d0;
}

static uint32_t dc_off8(dcs_t *s, int32_t pos)
{
    uint32_t d0;
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L0CD2;
    d0 = 7;
    d0 = dc_gb(s, d0);
    d0 += 1;
    goto out;
L0CD2:
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L0CF8;
    d0 = 9;
    d0 = dc_gbl(s, d0);
    d0 += 0x81;
    goto out;
L0CF8:
    if (pos > 642) goto L0D16;
    d0 = 1;
    d0 = dc_gb(s, d0);
    d0 += 0x281;
    goto out;
L0D16:
    if (pos > 644) goto L0D34;
    d0 = 2;
    d0 = dc_gb(s, d0);
    d0 += 0x281;
    goto out;
L0D34:
    if (pos > 648) goto L0D52;
    d0 = 3;
    d0 = dc_gb(s, d0);
    d0 += 0x281;
    goto out;
L0D52:
    if (pos > 656) goto L0D70;
    d0 = 4;
    d0 = dc_gb(s, d0);
    d0 += 0x281;
    goto out;
L0D70:
    if (pos > 672) goto L0D8E;
    d0 = 5;
    d0 = dc_gb(s, d0);
    d0 += 0x281;
    goto out;
L0D8E:
    if (pos > 704) goto L0DAC;
    d0 = 6;
    d0 = dc_gb(s, d0);
    d0 += 0x281;
    goto out;
L0DAC:
    if (pos > 768) goto L0DC8;
    d0 = 7;
    d0 = dc_gb(s, d0);
    d0 += 0x281;
    goto out;
L0DC8:
    if (pos > 896) goto L0DE4;
    d0 = 8;
    d0 = dc_gb(s, d0);
    d0 += 0x281;
    goto out;
L0DE4:
    if (pos > 1152) goto L0E00;
    d0 = 9;
    d0 = dc_gbl(s, d0);
    d0 += 0x281;
    goto out;
L0E00:
    if (pos > 1644) goto L0E1C;
    d0 = 10;
    d0 = dc_gbl(s, d0);
    d0 += 0x281;
    goto out;
L0E1C:
    d0 = 11;
    d0 = dc_gbl(s, d0);
    d0 += 0x281;
out:
    return d0;
}

static uint32_t dc_off9(dcs_t *s, int32_t pos)
{
    uint32_t d0;
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L0E6A;
    d0 = 8;
    d0 = dc_gb(s, d0);
    d0 += 1;
    goto out;
L0E6A:
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L0E90;
    d0 = 10;
    d0 = dc_gbl(s, d0);
    d0 += 0x101;
    goto out;
L0E90:
    if (pos > 1282) goto L0EAE;
    d0 = 1;
    d0 = dc_gb(s, d0);
    d0 += 0x501;
    goto out;
L0EAE:
    if (pos > 1284) goto L0ECC;
    d0 = 2;
    d0 = dc_gb(s, d0);
    d0 += 0x501;
    goto out;
L0ECC:
    if (pos > 1288) goto L0EEA;
    d0 = 3;
    d0 = dc_gb(s, d0);
    d0 += 0x501;
    goto out;
L0EEA:
    if (pos > 1296) goto L0F08;
    d0 = 4;
    d0 = dc_gb(s, d0);
    d0 += 0x501;
    goto out;
L0F08:
    if (pos > 1312) goto L0F26;
    d0 = 5;
    d0 = dc_gb(s, d0);
    d0 += 0x501;
    goto out;
L0F26:
    if (pos > 1344) goto L0F44;
    d0 = 6;
    d0 = dc_gb(s, d0);
    d0 += 0x501;
    goto out;
L0F44:
    if (pos > 1408) goto L0F62;
    d0 = 7;
    d0 = dc_gb(s, d0);
    d0 += 0x501;
    goto out;
L0F62:
    if (pos > 1536) goto L0F7E;
    d0 = 8;
    d0 = dc_gb(s, d0);
    d0 += 0x501;
    goto out;
L0F7E:
    if (pos > 1792) goto L0F9A;
    d0 = 9;
    d0 = dc_gbl(s, d0);
    d0 += 0x501;
    goto out;
L0F9A:
    if (pos > 2304) goto L0FB6;
    d0 = 10;
    d0 = dc_gbl(s, d0);
    d0 += 0x501;
    goto out;
L0FB6:
    if (pos > 3328) goto L0FD2;
    d0 = 11;
    d0 = dc_gbl(s, d0);
    d0 += 0x501;
    goto out;
L0FD2:
    d0 = 12;
    d0 = dc_gbl(s, d0);
    d0 += 0x501;
out:
    return d0;
}

static uint32_t dc_off10(dcs_t *s, int32_t pos)
{
    uint32_t d0;
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L1020;
    d0 = 9;
    d0 = dc_gbl(s, d0);
    d0 += 1;
    goto out;
L1020:
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L1046;
    d0 = 11;
    d0 = dc_gbl(s, d0);
    d0 += 0x201;
    goto out;
L1046:
    if (pos > 2562) goto L1064;
    d0 = 1;
    d0 = dc_gb(s, d0);
    d0 += 0xA01;
    goto out;
L1064:
    if (pos > 2564) goto L1082;
    d0 = 2;
    d0 = dc_gb(s, d0);
    d0 += 0xA01;
    goto out;
L1082:
    if (pos > 2568) goto L10A0;
    d0 = 3;
    d0 = dc_gb(s, d0);
    d0 += 0xA01;
    goto out;
L10A0:
    if (pos > 2576) goto L10BE;
    d0 = 4;
    d0 = dc_gb(s, d0);
    d0 += 0xA01;
    goto out;
L10BE:
    if (pos > 2592) goto L10DC;
    d0 = 5;
    d0 = dc_gb(s, d0);
    d0 += 0xA01;
    goto out;
L10DC:
    if (pos > 2624) goto L10FA;
    d0 = 6;
    d0 = dc_gb(s, d0);
    d0 += 0xA01;
    goto out;
L10FA:
    if (pos > 2688) goto L1118;
    d0 = 7;
    d0 = dc_gb(s, d0);
    d0 += 0xA01;
    goto out;
L1118:
    if (pos > 2816) goto L1136;
    d0 = 8;
    d0 = dc_gb(s, d0);
    d0 += 0xA01;
    goto out;
L1136:
    if (pos > 3072) goto L1152;
    d0 = 9;
    d0 = dc_gbl(s, d0);
    d0 += 0xA01;
    goto out;
L1152:
    if (pos > 3584) goto L116E;
    d0 = 10;
    d0 = dc_gbl(s, d0);
    d0 += 0xA01;
    goto out;
L116E:
    if (pos > 4608) goto L118A;
    d0 = 11;
    d0 = dc_gbl(s, d0);
    d0 += 0xA01;
    goto out;
L118A:
    if (pos > 6656) goto L11A6;
    d0 = 12;
    d0 = dc_gbl(s, d0);
    d0 += 0xA01;
    goto out;
L11A6:
    d0 = 13;
    d0 = dc_gbl(s, d0);
    d0 += 0xA01;
out:
    return d0;
}

static uint32_t dc_off11(dcs_t *s, int32_t pos)
{
    uint32_t d0;
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L11F4;
    d0 = 10;
    d0 = dc_gbl(s, d0);
    d0 += 1;
    goto out;
L11F4:
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L121A;
    d0 = 12;
    d0 = dc_gbl(s, d0);
    d0 += 0x401;
    goto out;
L121A:
    if (pos > 5122) goto L1238;
    d0 = 1;
    d0 = dc_gb(s, d0);
    d0 += 0x1401;
    goto out;
L1238:
    if (pos > 5124) goto L1256;
    d0 = 2;
    d0 = dc_gb(s, d0);
    d0 += 0x1401;
    goto out;
L1256:
    if (pos > 5128) goto L1274;
    d0 = 3;
    d0 = dc_gb(s, d0);
    d0 += 0x1401;
    goto out;
L1274:
    if (pos > 5136) goto L1292;
    d0 = 4;
    d0 = dc_gb(s, d0);
    d0 += 0x1401;
    goto out;
L1292:
    if (pos > 5152) goto L12B0;
    d0 = 5;
    d0 = dc_gb(s, d0);
    d0 += 0x1401;
    goto out;
L12B0:
    if (pos > 5184) goto L12CE;
    d0 = 6;
    d0 = dc_gb(s, d0);
    d0 += 0x1401;
    goto out;
L12CE:
    if (pos > 5248) goto L12EC;
    d0 = 7;
    d0 = dc_gb(s, d0);
    d0 += 0x1401;
    goto out;
L12EC:
    if (pos > 5376) goto L130A;
    d0 = 8;
    d0 = dc_gb(s, d0);
    d0 += 0x1401;
    goto out;
L130A:
    if (pos > 5632) goto L1328;
    d0 = 9;
    d0 = dc_gbl(s, d0);
    d0 += 0x1401;
    goto out;
L1328:
    if (pos > 6144) goto L1344;
    d0 = 10;
    d0 = dc_gbl(s, d0);
    d0 += 0x1401;
    goto out;
L1344:
    if (pos > 7168) goto L1360;
    d0 = 11;
    d0 = dc_gbl(s, d0);
    d0 += 0x1401;
    goto out;
L1360:
    if (pos > 9216) goto L137C;
    d0 = 12;
    d0 = dc_gbl(s, d0);
    d0 += 0x1401;
    goto out;
L137C:
    if (pos > 13312) goto L1398;
    d0 = 13;
    d0 = dc_gbl(s, d0);
    d0 += 0x1401;
    goto out;
L1398:
    d0 = 14;
    d0 = dc_gbl(s, d0);
    d0 += 0x1401;
out:
    return d0;
}

static uint32_t dc_off12(dcs_t *s, int32_t pos)
{
    uint32_t d0;
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L13E6;
    d0 = 11;
    d0 = dc_gbl(s, d0);
    d0 += 1;
    goto out;
L13E6:
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L140C;
    d0 = 13;
    d0 = dc_gbl(s, d0);
    d0 += 0x801;
    goto out;
L140C:
    if (pos > 10242) goto L142A;
    d0 = 1;
    d0 = dc_gb(s, d0);
    d0 += 0x2801;
    goto out;
L142A:
    if (pos > 10244) goto L1448;
    d0 = 2;
    d0 = dc_gb(s, d0);
    d0 += 0x2801;
    goto out;
L1448:
    if (pos > 10248) goto L1466;
    d0 = 3;
    d0 = dc_gb(s, d0);
    d0 += 0x2801;
    goto out;
L1466:
    if (pos > 10256) goto L1484;
    d0 = 4;
    d0 = dc_gb(s, d0);
    d0 += 0x2801;
    goto out;
L1484:
    if (pos > 10272) goto L14A2;
    d0 = 5;
    d0 = dc_gb(s, d0);
    d0 += 0x2801;
    goto out;
L14A2:
    if (pos > 10304) goto L14C0;
    d0 = 6;
    d0 = dc_gb(s, d0);
    d0 += 0x2801;
    goto out;
L14C0:
    if (pos > 10368) goto L14DE;
    d0 = 7;
    d0 = dc_gb(s, d0);
    d0 += 0x2801;
    goto out;
L14DE:
    if (pos > 10496) goto L14FC;
    d0 = 8;
    d0 = dc_gb(s, d0);
    d0 += 0x2801;
    goto out;
L14FC:
    if (pos > 10752) goto L151A;
    d0 = 9;
    d0 = dc_gbl(s, d0);
    d0 += 0x2801;
    goto out;
L151A:
    if (pos > 11264) goto L1538;
    d0 = 10;
    d0 = dc_gbl(s, d0);
    d0 += 0x2801;
    goto out;
L1538:
    if (pos > 12288) goto L1554;
    d0 = 11;
    d0 = dc_gbl(s, d0);
    d0 += 0x2801;
    goto out;
L1554:
    if (pos > 14336) goto L1570;
    d0 = 12;
    d0 = dc_gbl(s, d0);
    d0 += 0x2801;
    goto out;
L1570:
    if (pos > 18432) goto L158C;
    d0 = 13;
    d0 = dc_gbl(s, d0);
    d0 += 0x2801;
    goto out;
L158C:
    if (pos > 26624) goto L15A8;
    d0 = 14;
    d0 = dc_gbl(s, d0);
    d0 += 0x2801;
    goto out;
L15A8:
    d0 = 15;
    d0 = dc_gbl(s, d0);
    d0 += 0x2801;
out:
    return d0;
}

static uint32_t dc_off13(dcs_t *s, int32_t pos)
{
    uint32_t d0;
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L15F6;
    d0 = 12;
    d0 = dc_gbl(s, d0);
    d0 += 1;
    goto out;
L15F6:
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L161C;
    d0 = 14;
    d0 = dc_gbl(s, d0);
    d0 += 0x1001;
    goto out;
L161C:
    if (pos > 20482) goto L163A;
    d0 = 1;
    d0 = dc_gb(s, d0);
    d0 += 0x5001;
    goto out;
L163A:
    if (pos > 20484) goto L1658;
    d0 = 2;
    d0 = dc_gb(s, d0);
    d0 += 0x5001;
    goto out;
L1658:
    if (pos > 20488) goto L1676;
    d0 = 3;
    d0 = dc_gb(s, d0);
    d0 += 0x5001;
    goto out;
L1676:
    if (pos > 20496) goto L1694;
    d0 = 4;
    d0 = dc_gb(s, d0);
    d0 += 0x5001;
    goto out;
L1694:
    if (pos > 20512) goto L16B2;
    d0 = 5;
    d0 = dc_gb(s, d0);
    d0 += 0x5001;
    goto out;
L16B2:
    if (pos > 20544) goto L16D0;
    d0 = 6;
    d0 = dc_gb(s, d0);
    d0 += 0x5001;
    goto out;
L16D0:
    if (pos > 20608) goto L16EE;
    d0 = 7;
    d0 = dc_gb(s, d0);
    d0 += 0x5001;
    goto out;
L16EE:
    if (pos > 20736) goto L170C;
    d0 = 8;
    d0 = dc_gb(s, d0);
    d0 += 0x5001;
    goto out;
L170C:
    if (pos > 20992) goto L172A;
    d0 = 9;
    d0 = dc_gbl(s, d0);
    d0 += 0x5001;
    goto out;
L172A:
    if (pos > 21504) goto L1748;
    d0 = 10;
    d0 = dc_gbl(s, d0);
    d0 += 0x5001;
    goto out;
L1748:
    if (pos > 22528) goto L1766;
    d0 = 11;
    d0 = dc_gbl(s, d0);
    d0 += 0x5001;
    goto out;
L1766:
    if (pos > 24576) goto L1782;
    d0 = 12;
    d0 = dc_gbl(s, d0);
    d0 += 0x5001;
    goto out;
L1782:
    if (pos > 28672) goto L179E;
    d0 = 13;
    d0 = dc_gbl(s, d0);
    d0 += 0x5001;
    goto out;
L179E:
    if (pos > 36864) goto L17BA;
    d0 = 14;
    d0 = dc_gbl(s, d0);
    d0 += 0x5001;
    goto out;
L17BA:
    if (pos > 53248) goto L17D6;
    d0 = 15;
    d0 = dc_gbl(s, d0);
    d0 += 0x5001;
    goto out;
L17D6:
    d0 = 16;
    d0 = dc_gbl(s, d0);
    d0 += 0x5001;
out:
    return d0;
}

static uint32_t dc_off14(dcs_t *s, int32_t pos)
{
    uint32_t d0;
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L1824;
    d0 = 13;
    d0 = dc_gbl(s, d0);
    d0 += 1;
    goto out;
L1824:
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L184A;
    d0 = 15;
    d0 = dc_gbl(s, d0);
    d0 += 0x2001;
    goto out;
L184A:
    if (pos > 40962) goto L1868;
    d0 = 1;
    d0 = dc_gb(s, d0);
    d0 += 0xA001;
    goto out;
L1868:
    if (pos > 40964) goto L1886;
    d0 = 2;
    d0 = dc_gb(s, d0);
    d0 += 0xA001;
    goto out;
L1886:
    if (pos > 40968) goto L18A4;
    d0 = 3;
    d0 = dc_gb(s, d0);
    d0 += 0xA001;
    goto out;
L18A4:
    if (pos > 40976) goto L18C2;
    d0 = 4;
    d0 = dc_gb(s, d0);
    d0 += 0xA001;
    goto out;
L18C2:
    if (pos > 40992) goto L18E0;
    d0 = 5;
    d0 = dc_gb(s, d0);
    d0 += 0xA001;
    goto out;
L18E0:
    if (pos > 41024) goto L18FE;
    d0 = 6;
    d0 = dc_gb(s, d0);
    d0 += 0xA001;
    goto out;
L18FE:
    if (pos > 41088) goto L191C;
    d0 = 7;
    d0 = dc_gb(s, d0);
    d0 += 0xA001;
    goto out;
L191C:
    if (pos > 41216) goto L193A;
    d0 = 8;
    d0 = dc_gb(s, d0);
    d0 += 0xA001;
    goto out;
L193A:
    if (pos > 41472) goto L1958;
    d0 = 9;
    d0 = dc_gbl(s, d0);
    d0 += 0xA001;
    goto out;
L1958:
    if (pos > 41984) goto L1976;
    d0 = 10;
    d0 = dc_gbl(s, d0);
    d0 += 0xA001;
    goto out;
L1976:
    if (pos > 43008) goto L1994;
    d0 = 11;
    d0 = dc_gbl(s, d0);
    d0 += 0xA001;
    goto out;
L1994:
    if (pos > 45056) goto L19B2;
    d0 = 12;
    d0 = dc_gbl(s, d0);
    d0 += 0xA001;
    goto out;
L19B2:
    if (pos > 49152) goto L19CE;
    d0 = 13;
    d0 = dc_gbl(s, d0);
    d0 += 0xA001;
    goto out;
L19CE:
    if (pos > 57344) goto L19EA;
    d0 = 14;
    d0 = dc_gbl(s, d0);
    d0 += 0xA001;
    goto out;
L19EA:
    if (pos > 73728) goto L1A06;
    d0 = 15;
    d0 = dc_gbl(s, d0);
    d0 += 0xA001;
    goto out;
L1A06:
    if (pos > 106496) goto L1A22;
    d0 = 16;
    d0 = dc_gbl(s, d0);
    d0 += 0xA001;
    goto out;
L1A22:
    d0 = 17;
    d0 = dc_gbl(s, d0);
    d0 += 0xA001;
out:
    return d0;
}

static uint32_t dc_off15(dcs_t *s, int32_t pos)
{
    uint32_t d0;
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L1A70;
    d0 = 14;
    d0 = dc_gbl(s, d0);
    d0 += 1;
    goto out;
L1A70:
    d0 = 1;
    d0 = dc_gb(s, d0);
    if (d0 != 0) goto L1A96;
    d0 = 16;
    d0 = dc_gbl(s, d0);
    d0 += 0x4001;
    goto out;
L1A96:
    if (pos > 81922) goto L1AB4;
    d0 = 1;
    d0 = dc_gb(s, d0);
    d0 += 0x14001;
    goto out;
L1AB4:
    if (pos > 81924) goto L1AD2;
    d0 = 2;
    d0 = dc_gb(s, d0);
    d0 += 0x14001;
    goto out;
L1AD2:
    if (pos > 81928) goto L1AF0;
    d0 = 3;
    d0 = dc_gb(s, d0);
    d0 += 0x14001;
    goto out;
L1AF0:
    if (pos > 81936) goto L1B0E;
    d0 = 4;
    d0 = dc_gb(s, d0);
    d0 += 0x14001;
    goto out;
L1B0E:
    if (pos > 81952) goto L1B2C;
    d0 = 5;
    d0 = dc_gb(s, d0);
    d0 += 0x14001;
    goto out;
L1B2C:
    if (pos > 81984) goto L1B4A;
    d0 = 6;
    d0 = dc_gb(s, d0);
    d0 += 0x14001;
    goto out;
L1B4A:
    if (pos > 8204) goto L1B68;
    d0 = 7;
    d0 = dc_gb(s, d0);
    d0 += 0x14001;
    goto out;
L1B68:
    if (pos > 82176) goto L1B86;
    d0 = 8;
    d0 = dc_gb(s, d0);
    d0 += 0x14001;
    goto out;
L1B86:
    if (pos > 82432) goto L1BA4;
    d0 = 9;
    d0 = dc_gbl(s, d0);
    d0 += 0x14001;
    goto out;
L1BA4:
    if (pos > 82944) goto L1BC2;
    d0 = 10;
    d0 = dc_gbl(s, d0);
    d0 += 0x14001;
    goto out;
L1BC2:
    if (pos > 83968) goto L1BE0;
    d0 = 11;
    d0 = dc_gbl(s, d0);
    d0 += 0x14001;
    goto out;
L1BE0:
    if (pos > 86016) goto L1BFE;
    d0 = 12;
    d0 = dc_gbl(s, d0);
    d0 += 0x14001;
    goto out;
L1BFE:
    if (pos > 90112) goto L1C1C;
    d0 = 13;
    d0 = dc_gbl(s, d0);
    d0 += 0x14001;
    goto out;
L1C1C:
    if (pos > 98304) goto L1C38;
    d0 = 14;
    d0 = dc_gbl(s, d0);
    d0 += 0x14001;
    goto out;
L1C38:
    if (pos > 114688) goto L1C54;
    d0 = 15;
    d0 = dc_gbl(s, d0);
    d0 += 0x14001;
    goto out;
L1C54:
    if (pos > 147456) goto L1C70;
    d0 = 16;
    d0 = dc_gbl(s, d0);
    d0 += 0x14001;
    goto out;
L1C70:
    if (pos > 212992) goto L1C8C;
    d0 = 17;
    d0 = dc_gbl(s, d0);
    d0 += 0x14001;
    goto out;
L1C8C:
    d0 = 18;
    d0 = dc_gbl(s, d0);
    d0 += 0x14001;
out:
    return d0;
}


/* $90E0 offset code: the number of offset bits grows with the output
 * position (the window is the output so far). */
static uint32_t dc_off(dcs_t *s, int32_t pos)
{
    if (pos <= 10)      return dc_off1(s, pos);
    if (pos <= 20)      return dc_off2(s, pos);
    if (pos <= 40)      return dc_off3(s, pos);
    if (pos <= 80)      return dc_off4(s, pos);
    if (pos <= 0xA0)    return dc_off5(s, pos);
    if (pos <= 0x2A0)   return dc_off6(s, pos);
    if (pos <= 0x3E8)   return dc_off7(s, pos);
    if (pos <= 0xA80)   return dc_off8(s, pos);
    if (pos <= 0x1500)  return dc_off9(s, pos);
    if (pos <= 0x2A00)  return dc_off10(s, pos);
    if (pos <= 0x5400)  return dc_off11(s, pos);
    if (pos <= 0xA800)  return dc_off12(s, pos);
    if (pos <= 0x11170) return dc_off13(s, pos);
    if (pos <= 0x2A000) return dc_off14(s, pos);
    return dc_off15(s, pos);
}

typedef struct { uint32_t d0, d1, d2, xflag; } dcres_t;

/* $8D38 (called by entry 1): decompress until size bytes are out */
static void dc_decompress(uint32_t src, uint32_t dst, uint32_t size,
                          uint32_t d1_in, uint32_t d2_in, dcres_t *r)
{
    dcs_t s;
    s.ptr = src; s.buf = 0; s.bytes = 0; s.cnt = 0; s.total = 0;
    s.d1 = d1_in; s.d2 = d2_in;
    uint32_t pos = 0, d0 = 0;
    uint32_t sz4 = size + 4, four = 4;
    uint32_t xflag = CFLAG_ADD_32(four, size, sz4);     /* ADDQ.L #4,D0 */
    int w2 = 1;
    uint32_t a4 = dst;
    while (pos < size) {
        uint32_t v = dc_len(&s);
        int32_t lc;
        if (v != 0 || w2 == 0) {
            lc = (int32_t)v + 2;
            if (w2 == 0) lc++;
            w2 = 1;
            uint32_t off = dc_off(&s, (int32_t)pos);
            uint32_t a3 = a4 - off;
            pos += (uint32_t)lc;
            /* byte-by-byte forward copy (overlap = run) */
            do {
                int32_t n = lc < 8 ? lc : 8;
                for (int32_t i = 0; i < n; i++)
                    dc_wr8(a4++, dc_rd8(a3++));
                lc -= 8;
            } while (lc > 0);
        } else {
            int16_t w8 = (int16_t)dc_litcount(&s);
            w2 = (w8 >= 63);
            pos += (uint32_t)(int32_t)w8;
            dc_copybytes(&s, a4, (int32_t)w8);
            a4 += (uint32_t)(int32_t)w8;
        }
        d0 = s.bytes + src;
        xflag = CFLAG_ADD_32(src, s.bytes, d0);
    }
    r->d0 = d0; r->d1 = s.d1; r->d2 = s.d2; r->xflag = xflag;
    mac_native_dcmp_bytes += pos;
}

/* Native entry 1. At the trap, A7 -> return address, then hdr, dst, src. */
static void dc_native_entry1(void)
{
    uint32_t sp = REG_A[7];
    uint32_t ret = dc_rd32(sp), hdr = dc_rd32(sp + 4);
    uint32_t dst = dc_rd32(sp + 8), src = dc_rd32(sp + 12);
    uint32_t size = dc_rd32(hdr + 8);
    dcres_t r;
    dc_decompress(src, dst, size, REG_D[1], REG_D[2], &r);
    /* epilogue: CLR.W $14(A6) ; UNLK A6 ; MOVEA.L (A7)+,A0 ;
     *           LEA $C(A7),A7 ; JMP (A0) */
    dc_wr8(sp + 0x10, 0); dc_wr8(sp + 0x11, 0);
    REG_D[0] = r.d0; REG_D[1] = r.d1; REG_D[2] = r.d2;
    REG_A[0] = ret;
    REG_A[7] = sp + 16;
    FLAG_X = r.xflag;
    FLAG_N = NFLAG_CLEAR; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR; FLAG_Z = ZFLAG_SET;
    REG_PC = ret;
    mac_native_dcmp_count++;
}

#if MAC_NATIVE_DCMP_CHECK
#include <stdio.h>
#include <stdlib.h>
/* Host check: run native into the real RAM, keep a copy of the result,
 * restore, run the original 68k code nested (one instruction per
 * m68k_execute(1), interrupts held off) to its return, and compare. */
static void dc_check_entry1(uint32_t code)
{
    static uint8_t *pre, *nat;
    if (!pre) { pre = malloc(dc_ram_size); nat = malloc(dc_ram_size); }
    m68ki_cpu_core ctx0, ctxn;
    memcpy(&ctx0, &m68ki_cpu, sizeof ctx0);
    sint rem0 = m68ki_remaining_cycles;
    extern int m68ki_initial_cycles;
    int init0 = m68ki_initial_cycles;
    uint32_t sp = REG_A[7], ret = dc_rd32(sp);
    memcpy(pre, dc_ram, dc_ram_size);

    dc_native_entry1();
    memcpy(&ctxn, &m68ki_cpu, sizeof ctxn);
    memcpy(nat, dc_ram, dc_ram_size);

    /* 68k reference */
    memcpy(dc_ram, pre, dc_ram_size);
    memcpy(&m68ki_cpu, &ctx0, sizeof ctx0);
    dc_ram[code + DC_ENTRY1] = DC_OP1 >> 8; dc_ram[code + DC_ENTRY1 + 1] = DC_OP1 & 0xFF;
    REG_PC = code + DC_ENTRY1 + (ctx0.pc & 0xFF000000u);
    uint saved_int = CPU_INT_LEVEL;
    CPU_INT_LEVEL = 0;
    unsigned long n = 0;
    while (!((REG_PC & 0xFFFFFF) == (ret & 0xFFFFFF) && REG_A[7] == sp + 16)) {
        m68k_execute(1);
        if (++n > 200000000ul) { fprintf(stderr, "H dcmp-check: runaway\n"); break; }
    }
    CPU_INT_LEVEL = saved_int;
    dc_ram[code + DC_ENTRY1] = DC_TRAP >> 8; dc_ram[code + DC_ENTRY1 + 1] = DC_TRAP & 0xFF;

    /* compare: RAM except the dead stack below the entry SP, CPU state */
    int bad = 0;
    uint32_t lo = (sp > 0x1000 ? sp - 0x1000 : 0) & 0xFFFFFF, hi = sp & 0xFFFFFF;
    for (uint32_t a = 0; a < dc_ram_size; a++) {
        if (a >= lo && a < hi) continue;
        if (dc_ram[a] != nat[a]) {
            if (bad < 8) fprintf(stderr, "H dcmp-check: RAM $%06X 68k=%02X native=%02X\n",
                                 a, dc_ram[a], nat[a]);
            bad++;
        }
    }
    for (int i = 0; i < 16; i++)
        if (m68ki_cpu.dar[i] != ctxn.dar[i]) {
            fprintf(stderr, "H dcmp-check: %c%d 68k=%08X native=%08X\n",
                    i < 8 ? 'D' : 'A', i & 7, m68ki_cpu.dar[i], ctxn.dar[i]);
            bad++;
        }
    if ((m68ki_cpu.pc & 0xFFFFFF) != (ctxn.pc & 0xFFFFFF) ||
        !!(FLAG_X & 0x100) != !!(ctxn.x_flag & 0x100) ||
        !!(FLAG_N & 0x80) != !!(ctxn.n_flag & 0x80) ||
        !FLAG_Z != !ctxn.not_z_flag ||
        !!(FLAG_V & 0x80) != !!(ctxn.v_flag & 0x80) ||
        !!(FLAG_C & 0x100) != !!(ctxn.c_flag & 0x100)) {
        fprintf(stderr, "H dcmp-check: PC/flags 68k PC=%08X X%d N%d Z%d V%d C%d native PC=%08X X%d N%d Z%d V%d C%d\n",
                m68ki_cpu.pc, !!(FLAG_X & 0x100), !!(FLAG_N & 0x80), !FLAG_Z, !!(FLAG_V & 0x80), !!(FLAG_C & 0x100),
                ctxn.pc, !!(ctxn.x_flag & 0x100), !!(ctxn.n_flag & 0x80), !ctxn.not_z_flag,
                !!(ctxn.v_flag & 0x80), !!(ctxn.c_flag & 0x100));
        bad++;
    }
    if (bad) mac_native_dcmp_bad++;
    /* keep the native result (same timing as native mode) */
    memcpy(dc_ram, nat, dc_ram_size);
    memcpy(&m68ki_cpu, &ctxn, sizeof ctxn);
    m68ki_remaining_cycles = rem0;
    m68ki_initial_cycles = init0;
    if (n > 0) fprintf(stderr, "H dcmp-check: call %u size %u: %s (%lu instr)\n",
                       mac_native_dcmp_count, dc_rd32(dc_rd32(sp + 4) + 8), bad ? "MISMATCH" : "ok", n);
}
#endif

/* The patched word at entry 1 was executed: REG_PC = entry1 + 2. */
static __attribute__((noinline)) int dc_trap(void)
{
    if (!dc_ram) { dc_ram = mac_mem_get_ram(); dc_ram_size = mac_mem_get_ram_size(); }
    uint32_t e1 = (REG_PC - 2) & 0xFFFFFF;
    uint32_t code = e1 - DC_ENTRY1;
    if (e1 < DC_ENTRY1 || code + DC_LEN > dc_ram_size)
        return 0;
    if (dc_hash(code, DC_ENTRY1, DC_OP1) != DC_HASH) {
        /* Not (or no longer) our code: if the word after the trap is
         * entry 1's, undo the patch and let the interpreter run it. */
        if (dc_ram[e1 + 2] == 0xFF && dc_ram[e1 + 3] == 0xD6) {
            dc_ram[e1] = DC_OP1 >> 8; dc_ram[e1 + 1] = DC_OP1 & 0xFF;
            REG_PC -= 2;
            return 1;
        }
        return 0;
    }
#if MAC_NATIVE_DCMP_CHECK
    dc_check_entry1(code);
#else
    dc_native_entry1();
#endif
    return 1;
}

/* Get1Resource('dcmp', 3) about to run (glue): patch entry 1 of the
 * loaded dcmp, found through CurMap's resource map. */
static __attribute__((noinline)) void dc_maybe_patch(void)
{
    if (!dc_ram) { dc_ram = mac_mem_get_ram(); dc_ram_size = mac_mem_get_ram_size(); }
    uint32_t sp = REG_A[7];
    if (dc_rd32(sp + 2) != 0x64636D70u || dc_rd16(sp) != 3) return;   /* 'dcmp', 3 */
    uint32_t cur = dc_rd16(0x0A5A);                                  /* CurMap */
    uint32_t mh = dc_rd32(0x0A50) & 0xFFFFFF;                        /* TopMapHndl */
    for (int g = 0; mh && g < 64; g++) {
        uint32_t mp = dc_rd32(mh) & 0xFFFFFF;
        if (!mp || mp + 30 > dc_ram_size) return;
        if (dc_rd16(mp + 20) != cur) { mh = dc_rd32(mp + 16) & 0xFFFFFF; continue; }
        uint32_t tl = mp + dc_rd16(mp + 24);
        uint32_t nt = (dc_rd16(tl) + 1) & 0xFFFF;
        for (uint32_t t = 0; t < nt && t < 4096; t++) {
            uint32_t te = tl + 2 + t * 8;
            if (dc_rd32(te) != 0x64636D70u) continue;
            uint32_t nr = (dc_rd16(te + 4) + 1) & 0xFFFF;
            uint32_t rl = tl + dc_rd16(te + 6);
            for (uint32_t i = 0; i < nr && i < 4096; i++) {
                uint32_t re = rl + i * 12;
                if (dc_rd16(re) != 3) continue;
                uint32_t h = dc_rd32(re + 8) & 0xFFFFFF;
                if (!h || h + 4 > dc_ram_size) return;
                uint32_t code = dc_rd32(h) & 0xFFFFFF;
                if (!code || code + DC_LEN > dc_ram_size) return;
                uint32_t w = (dc_ram[code + DC_ENTRY1] << 8) | dc_ram[code + DC_ENTRY1 + 1];
                if (w == DC_TRAP) return;                   /* already patched */
                if (w == DC_OP1 && dc_hash(code, DC_LEN, 0) == DC_HASH) {
                    dc_ram[code + DC_ENTRY1] = DC_TRAP >> 8;
                    dc_ram[code + DC_ENTRY1 + 1] = DC_TRAP & 0xFF;
                    mac_native_dcmp_patches++;
                }
                return;
            }
            return;
        }
        return;
    }
}

#endif /* MAC_NATIVE_DCMP */

/* ======================================================================== */
/* A-line pre-hook: called by m68ki_exception_1010() before the exception   */
/* is taken. Returns 1 if the instruction was handled (no exception).       */
/* ======================================================================== */
#if MAC_NATIVE_DCMP
extern int mac_quit_repaint_pre(unsigned int ir);   /* mac_emulator.c */
#if MAC_ROM_LC_II && defined(MAC_DRIVE2) && MAC_DRIVE2
extern int v8disk_aline_pre(unsigned int ir);       /* mac_v8disk.c */
#endif

int mac_native_aline_pre(unsigned int ir)
{
#if MAC_ROM_LC_II && defined(MAC_DRIVE2) && MAC_DRIVE2
    if (v8disk_aline_pre(ir)) return 1;
#endif
    if (mac_quit_repaint_pre(ir)) return 1;
    if (ir == DC_TRAP) return dc_trap();
    if (ir == 0xA81F) dc_maybe_patch();
    return 0;
}
#endif

#ifdef HOST_BUILD
#include <stdio.h>
#include <stdlib.h>
static void mac_native_report(void)
{
#if MAC_NATIVE_TRAPDISP
    fprintf(stderr, "H native: trap dispatches run natively: %u\n", mac_native_disp_count);
#endif
#if MAC_NATIVE_DCMP
    fprintf(stderr, "H native: dcmp 3 calls %u (%u bytes), patches %u\n",
            mac_native_dcmp_count, mac_native_dcmp_bytes, mac_native_dcmp_patches);
#if MAC_NATIVE_DCMP_CHECK
    fprintf(stderr, "H native: dcmp-check mismatches %u\n", mac_native_dcmp_bad);
#endif
#endif
#if MAC_NATIVE_LOOPS
    {
        extern unsigned int mac_native_loop_instr;
        fprintf(stderr, "H native: ROM loop instructions run natively: %u\n", mac_native_loop_instr);
    }
#endif
}
__attribute__((constructor)) static void mac_native_report_init(void)
{
    atexit(mac_native_report);
}
#endif

/* ======================================================================== */
/* 3. MAC_NATIVE_LOOPS: long-running ROM loops, entered at slice start.     */
/*                                                                          */
/* m68k_execute() calls mac_native_slice() once per time slice, after the   */
/* interrupt check. If the PC is inside one of the loops below (fixed ROM   */
/* addresses, ROM bytes verified once through the fetch path), the loop's   */
/* instructions run here with Musashi's exact semantics: same memory        */
/* accesses through m68ki_read/write, same flags, same cycle charge per     */
/* instruction (USE_CYCLES as the interpreter does), and the same "run an   */
/* instruction while cycles remain" rule, so the slice ends on exactly the  */
/* same instruction. The CPU leaves the loop either at the slice end (the   */
/* next slice re-enters here) or at the loop exit (the interpreter goes on).*/
/* A loop entered in the middle of a slice is interpreted until that slice  */
/* ends; these loops run for millions of instructions, so that is noise.   */
/* None of them touches devices, so instruction-internal timing does not    */
/* matter.                                                                   */
/*   LC II  $A468C4  startup RAM test (16x MOVE.L (A2)+,D2 / EOR.L D2,(A2))  */
/*   LC II  $A46AF0  startup ROM checksum (word sum)                         */
/*   LC II  $A47080  timeout loop waiting for an interrupt to clear D1       */
/*   LC II  $A0E148  Memory Manager heap walk (find free block)              */
/*   Classic $41A84A SCSI-reset delay (DBRA D0 / DBRA D5)                    */
/* ======================================================================== */
#if MAC_NATIVE_LOOPS

extern unsigned int m68k_read_immediate_16(unsigned int address);

typedef struct {
    uint32_t lo, len;                   /* 24-bit ROM address, bytes */
    const uint16_t *words;              /* expected code */
    int8_t ok;                          /* -1 unknown, 0 no, 1 verified */
    int8_t v8;                          /* 1: LC II ROM (68030), 0: Classic ROM (68000) */
} nl_loop_t;

static const uint16_t nl_ramtest_w[] = {
    0x241A,0xB592,0x241A,0xB592,0x241A,0xB592,0x241A,0xB592,
    0x241A,0xB592,0x241A,0xB592,0x241A,0xB592,0x241A,0xB592,
    0x241A,0xB592,0x241A,0xB592,0x241A,0xB592,0x241A,0xB592,
    0x241A,0xB592,0x241A,0xB592,0x241A,0xB592,0x241A,0xB592,
    0x51CB,0xFFBE,0x0483,0x0001,0x0000,0x6AB4 };
static const uint16_t nl_romsum_w[] = { 0x3018,0xD280,0x5383,0x66F8 };
static const uint16_t nl_wait_w[]   = { 0x4A41,0x6706,0x5380,0x66F8 };
static const uint16_t nl_heap_w[]   = { 0xB3D6,0x6426,0x2211,0x4A11,0x6706,0xC282,0xD3C1,0x60F0 };
static const uint16_t nl_delay_w[]  = { 0x51C8,0xFFFE,0x51CD,0xFFFA };

enum { NL_RAMTEST, NL_ROMSUM, NL_WAIT, NL_HEAP, NL_DELAY, NL_N };
static nl_loop_t nl_loops[NL_N] = {
    { 0xA468C4, sizeof nl_ramtest_w, nl_ramtest_w, -1, 1 },
    { 0xA46AF0, sizeof nl_romsum_w,  nl_romsum_w,  -1, 1 },
    { 0xA47080, sizeof nl_wait_w,    nl_wait_w,    -1, 1 },
    { 0xA0E148, sizeof nl_heap_w,    nl_heap_w,    -1, 1 },
    { 0x41A84A, sizeof nl_delay_w,   nl_delay_w,   -1, 0 },
};
unsigned int mac_native_loop_instr;     /* instructions run natively */

static int nl_verify(nl_loop_t *l)
{
    if (l->ok < 0) {
        /* the address is ROM only on its own machine (68000 = Classic,
         * ROM $400000; 68030 = LC II, ROM $A00000); ROM never changes */
        l->ok = (l->v8 ? CPU_TYPE_IS_EC020_PLUS(CPU_TYPE) : CPU_TYPE_IS_000(CPU_TYPE)) ? 1 : 0;
        for (uint32_t i = 0; l->ok && i < l->len / 2; i++)
            if ((m68k_read_immediate_16(l->lo + 2 * i) & 0xFFFF) != l->words[i]) l->ok = 0;
    }
    return l->ok;
}

/* Before each instruction: the interpreter's "while cycles remain" test,
 * except for the first one of the slice (m68k_execute()'s loop is a
 * do-while: it always runs one instruction, even when the interrupt check
 * at slice start used up the slice).
 * NL_CY: the instruction's table cycles, charged as the interpreter does. */
#define NL_STEP(o)  do { if (n && GET_CYCLES() <= 0) { off = (o); goto out; } \
                         REG_PPC = base + (o); n++; } while (0)
#define NL_CY(op)   do { REG_IR = (op); USE_CYCLES(CYC_INSTRUCTION[(op)]); } while (0)
#define NL_NOTAKE() USE_CYCLES(CYC_BCC_NOTAKE_B)

/* LC II RAM test inner loop. Offsets: +00..+3E 16 x (MOVE.L (A2)+,D2 ;
 * EOR.L D2,(A2)), +40 DBRA D3,+00, +44 SUBI.L #$10000,D3, +4A BPL.S +00,
 * exit +4C. */
static uint nl_run_ramtest(uint base, uint off)
{
    uint *D = REG_D, *A = REG_A, res, ea, src, dst, n = 0;
    for (;;) {
        if (off < 0x40) {
            if (!(off & 2)) {
                NL_STEP(off);
                ea = A[2]; A[2] += 4; res = m68ki_read_32(ea); D[2] = res;
                FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
                NL_CY(0x241A);
            } else {
                NL_STEP(off);
                ea = A[2]; res = D[2] ^ m68ki_read_32(ea); m68ki_write_32(ea, res);
                FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_C = CFLAG_CLEAR; FLAG_V = VFLAG_CLEAR;
                NL_CY(0xB592);
            }
            off += 2;
        } else if (off == 0x40) {
            NL_STEP(0x40);
            res = MASK_OUT_ABOVE_16(D[3] - 1);
            D[3] = MASK_OUT_BELOW_16(D[3]) | res;
            REG_IR = 0x51CB;
            if (res != 0xFFFF) { USE_CYCLES(CYC_DBCC_F_NOEXP); off = 0; }
            else               { USE_CYCLES(CYC_DBCC_F_EXP);   off = 0x44; }
            USE_CYCLES(CYC_INSTRUCTION[0x51CB]);
        } else if (off == 0x44) {
            NL_STEP(0x44);
            src = 0x10000; dst = D[3]; res = dst - src;
            FLAG_N = NFLAG_32(res); FLAG_Z = MASK_OUT_ABOVE_32(res);
            FLAG_X = FLAG_C = CFLAG_SUB_32(src, dst, res);
            FLAG_V = VFLAG_SUB_32(src, dst, res);
            D[3] = FLAG_Z;
            NL_CY(0x0483);
            off = 0x4A;
        } else if (off == 0x4A) {
            NL_STEP(0x4A);
            REG_IR = 0x6AB4;
            if (COND_PL()) off = 0; else { NL_NOTAKE(); off = 0x4C; }
            USE_CYCLES(CYC_INSTRUCTION[0x6AB4]);
            if (off == 0x4C) goto out;
        } else {
            goto out;                   /* not an instruction boundary we know */
        }
    }
out:
    REG_PC = base + off;
    mac_native_loop_instr += n;
    return n;
}

/* LC II ROM checksum: +0 MOVE.W (A0)+,D0 ; +2 ADD.L D0,D1 ;
 * +4 SUBQ.L #1,D3 ; +6 BNE.S +0 ; exit +8 */
static uint nl_run_romsum(uint base, uint off)
{
    uint *D = REG_D, *A = REG_A, res, ea, src, dst, n = 0;
    switch (off) {
    case 0: goto i0;
    case 2: goto i2;
    case 4: goto i4;
    case 6: goto i6;
    default: goto out;
    }
    for (;;) {
i0:     NL_STEP(0);
        ea = A[0]; A[0] += 2; res = m68ki_read_16(ea);
        D[0] = MASK_OUT_BELOW_16(D[0]) | res;
        FLAG_N = NFLAG_16(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
        NL_CY(0x3018);
i2:     NL_STEP(2);
        src = D[0]; dst = D[1]; res = src + dst;
        FLAG_N = NFLAG_32(res); FLAG_V = VFLAG_ADD_32(src, dst, res);
        FLAG_X = FLAG_C = CFLAG_ADD_32(src, dst, res);
        FLAG_Z = MASK_OUT_ABOVE_32(res); D[1] = FLAG_Z;
        NL_CY(0xD280);
i4:     NL_STEP(4);
        src = 1; dst = D[3]; res = dst - src;
        FLAG_N = NFLAG_32(res); FLAG_Z = MASK_OUT_ABOVE_32(res);
        FLAG_X = FLAG_C = CFLAG_SUB_32(src, dst, res);
        FLAG_V = VFLAG_SUB_32(src, dst, res); D[3] = FLAG_Z;
        NL_CY(0x5383);
i6:     NL_STEP(6);
        REG_IR = 0x66F8;
        if (!COND_NE()) { NL_NOTAKE(); USE_CYCLES(CYC_INSTRUCTION[0x66F8]); off = 8; goto out; }
        USE_CYCLES(CYC_INSTRUCTION[0x66F8]);
    }
out:
    REG_PC = base + off;
    mac_native_loop_instr += n;
    return n;
}

/* LC II interrupt wait: +0 TST.W D1 ; +2 BEQ.S +$A (exit) ;
 * +4 SUBQ.L #1,D0 ; +6 BNE.S +0 ; exit +8 */
static uint nl_run_wait(uint base, uint off)
{
    uint *D = REG_D, res, src, dst, n = 0;
    switch (off) {
    case 0: goto i0;
    case 2: goto i2;
    case 4: goto i4;
    case 6: goto i6;
    default: goto out;
    }
    for (;;) {
i0:     NL_STEP(0);
        res = MASK_OUT_ABOVE_16(D[1]);
        FLAG_N = NFLAG_16(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
        NL_CY(0x4A41);
i2:     NL_STEP(2);
        REG_IR = 0x6706;
        if (COND_EQ()) { USE_CYCLES(CYC_INSTRUCTION[0x6706]); off = 0x0A; goto out; }
        NL_NOTAKE(); USE_CYCLES(CYC_INSTRUCTION[0x6706]);
i4:     NL_STEP(4);
        src = 1; dst = D[0]; res = dst - src;
        FLAG_N = NFLAG_32(res); FLAG_Z = MASK_OUT_ABOVE_32(res);
        FLAG_X = FLAG_C = CFLAG_SUB_32(src, dst, res);
        FLAG_V = VFLAG_SUB_32(src, dst, res); D[0] = FLAG_Z;
        NL_CY(0x5380);
i6:     NL_STEP(6);
        REG_IR = 0x66F8;
        if (!COND_NE()) { NL_NOTAKE(); USE_CYCLES(CYC_INSTRUCTION[0x66F8]); off = 8; goto out; }
        USE_CYCLES(CYC_INSTRUCTION[0x66F8]);
    }
out:
    REG_PC = base + off;
    mac_native_loop_instr += n;
    return n;
}

/* LC II heap walk: +0 CMPA.L (A6),A1 ; +2 BCC.S +$2A (exit) ;
 * +4 MOVE.L (A1),D1 ; +6 TST.B (A1) ; +8 BEQ.S +$10 (exit) ;
 * +A AND.L D2,D1 ; +C ADDA.L D1,A1 ; +E BRA.S +0 */
static uint nl_run_heap(uint base, uint off)
{
    uint *D = REG_D, *A = REG_A, res, src, dst, n = 0;
    switch (off) {
    case 0x0: goto i0;
    case 0x2: goto i2;
    case 0x4: goto i4;
    case 0x6: goto i6;
    case 0x8: goto i8;
    case 0xA: goto iA;
    case 0xC: goto iC;
    case 0xE: goto iE;
    default: goto out;
    }
    for (;;) {
i0:     NL_STEP(0x0);
        src = m68ki_read_32(A[6]); dst = A[1]; res = dst - src;
        FLAG_N = NFLAG_32(res); FLAG_Z = MASK_OUT_ABOVE_32(res);
        FLAG_V = VFLAG_SUB_32(src, dst, res); FLAG_C = CFLAG_SUB_32(src, dst, res);
        NL_CY(0xB3D6);
i2:     NL_STEP(0x2);
        REG_IR = 0x6426;
        if (COND_CC()) { USE_CYCLES(CYC_INSTRUCTION[0x6426]); off = 0x2A; goto out; }
        NL_NOTAKE(); USE_CYCLES(CYC_INSTRUCTION[0x6426]);
i4:     NL_STEP(0x4);
        res = m68ki_read_32(A[1]); D[1] = res;
        FLAG_N = NFLAG_32(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
        NL_CY(0x2211);
i6:     NL_STEP(0x6);
        res = m68ki_read_8(A[1]);
        FLAG_N = NFLAG_8(res); FLAG_Z = res; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
        NL_CY(0x4A11);
i8:     NL_STEP(0x8);
        REG_IR = 0x6706;
        if (COND_EQ()) { USE_CYCLES(CYC_INSTRUCTION[0x6706]); off = 0x10; goto out; }
        NL_NOTAKE(); USE_CYCLES(CYC_INSTRUCTION[0x6706]);
iA:     NL_STEP(0xA);
        FLAG_Z = D[1] &= D[2];
        FLAG_N = NFLAG_32(FLAG_Z); FLAG_C = CFLAG_CLEAR; FLAG_V = VFLAG_CLEAR;
        NL_CY(0xC282);
iC:     NL_STEP(0xC);
        A[1] = MASK_OUT_ABOVE_32(A[1] + D[1]);
        NL_CY(0xD3C1);
iE:     NL_STEP(0xE);
        NL_CY(0x60F0);                  /* BRA.S to +0 (not to itself) */
    }
out:
    REG_PC = base + off;
    mac_native_loop_instr += n;
    return n;
}

/* Classic SCSI-reset delay: +0 DBRA D0,+0 ; +4 DBRA D5,+0 ; exit +8 */
static uint nl_run_delay(uint base, uint off)
{
    uint *D = REG_D, res, n = 0;
    switch (off) {
    case 0: goto i0;
    case 4: goto i4;
    default: goto out;
    }
    for (;;) {
i0:     NL_STEP(0);
        res = MASK_OUT_ABOVE_16(D[0] - 1);
        D[0] = MASK_OUT_BELOW_16(D[0]) | res;
        REG_IR = 0x51C8;
        if (res != 0xFFFF) { USE_CYCLES(CYC_DBCC_F_NOEXP + CYC_INSTRUCTION[0x51C8]); continue; }
        USE_CYCLES(CYC_DBCC_F_EXP + CYC_INSTRUCTION[0x51C8]);
i4:     NL_STEP(4);
        res = MASK_OUT_ABOVE_16(D[5] - 1);
        D[5] = MASK_OUT_BELOW_16(D[5]) | res;
        REG_IR = 0x51CD;
        if (res != 0xFFFF) { USE_CYCLES(CYC_DBCC_F_NOEXP + CYC_INSTRUCTION[0x51CD]); continue; }
        USE_CYCLES(CYC_DBCC_F_EXP + CYC_INSTRUCTION[0x51CD]);
        off = 8;
        goto out;
    }
out:
    REG_PC = base + off;
    mac_native_loop_instr += n;
    return n;
}

/* Returns the number of instructions run natively (0: none, the
 * interpreter runs the slice as usual). */
int mac_native_slice(void)
{
    uint pc = REG_PC, pc24 = pc & 0xFFFFFF;
    /* quick reject: only the two ROM windows hold candidate loops */
    if (pc24 - 0xA0E148u >= 0x39000u && pc24 - 0x41A84Au >= 8u)
        return 0;
    for (int i = 0; i < NL_N; i++) {
        nl_loop_t *l = &nl_loops[i];
        uint off = pc24 - l->lo;
        if (off >= l->len || (off & 1) || !nl_verify(l)) continue;
        uint base = pc - off;
        switch (i) {
        case NL_RAMTEST: return (int)nl_run_ramtest(base, off);
        case NL_ROMSUM:  return (int)nl_run_romsum(base, off);
        case NL_WAIT:    return (int)nl_run_wait(base, off);
        case NL_HEAP:    return (int)nl_run_heap(base, off);
        case NL_DELAY:   return (int)nl_run_delay(base, off);
        }
        return 0;
    }
    return 0;
}

#endif /* MAC_NATIVE_LOOPS */
