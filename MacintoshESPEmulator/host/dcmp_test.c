/* Offline check of the native 'dcmp' 3 decompressor (MAC_NATIVE_DCMP,
 * components/musashi/mac_native.c) against the original 68k code.
 *
 *   make dcmp-test            (or: ./build/dcmp_test DISK.dsk [DISK.dsk...])
 *
 * The 68k decompressor is taken from the first disk image (the System file
 * holds 'dcmp' 3 uncompressed; it is found by its entry-1 bytes and checked
 * against the hash mac_native.c expects). Every compressed resource with
 * header version 9 and dcmp ID 3 found on the images is then decompressed
 * twice, in-place as the System's glue does it (compressed data at the end
 * of the destination buffer), on both a 68000 and a 68030:
 *   1. by the original code, interpreted by Musashi one instruction at a time,
 *   2. by dc_trap() in mac_native.c, from the same starting state,
 * and the complete RAM (except the dead stack below the caller's SP), all 16
 * registers, the PC and the condition codes are compared. Exit status 1 on
 * any mismatch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define RAMSZ 0x400000u
static uint8_t RAM[RAMSZ];
uint8_t *mac_mem_get_ram(void) { return RAM; }
uint32_t mac_mem_get_ram_size(void) { return RAMSZ; }

#include "mac_native.c"

static inline uint32_t A24(uint32_t a) { return a & 0xFFFFFF; }
unsigned int m68k_read_memory_8(unsigned int a)  { a = A24(a); return a < RAMSZ ? RAM[a] : 0; }
unsigned int m68k_read_memory_16(unsigned int a) { return (m68k_read_memory_8(a) << 8) | m68k_read_memory_8(a + 1); }
unsigned int m68k_read_memory_32(unsigned int a) { return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2); }
void m68k_write_memory_8(unsigned int a, unsigned int v)  { a = A24(a); if (a < RAMSZ) RAM[a] = (uint8_t)v; }
void m68k_write_memory_16(unsigned int a, unsigned int v) { m68k_write_memory_8(a, v >> 8); m68k_write_memory_8(a + 1, v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { m68k_write_memory_16(a, v >> 16); m68k_write_memory_16(a + 2, v); }
unsigned int m68k_read_immediate_16(unsigned int a)  { return m68k_read_memory_16(a); }
unsigned int m68k_read_immediate_32(unsigned int a)  { return m68k_read_memory_32(a); }
unsigned int m68k_read_pcrelative_8(unsigned int a)  { return m68k_read_memory_8(a); }
unsigned int m68k_read_pcrelative_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_pcrelative_32(unsigned int a) { return m68k_read_memory_32(a); }
unsigned int m68k_read_disassembler_8(unsigned int a)  { return m68k_read_memory_8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
void mac_aline_hook(unsigned int w, unsigned int pc) { (void)w; (void)pc; }
int mac_int_ack_handler(int l) { (void)l; return M68K_INT_ACK_AUTOVECTOR; }
void mac_reset_handler(void) {}
void host_trace_hook(unsigned int pc) { (void)pc; }

#define CODE 0x8CE8u                    /* where the Classic loads it */
#define HDR  0x20000u
#define BUF  0x100000u
#define STK  0x0F0000u
#define RET  0x3F0000u

static uint8_t code_img[DC_LEN], snap[RAMSZ], r68[RAMSZ];

static uint8_t *load(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); rewind(f);
    uint8_t *d = malloc(*n);
    if (fread(d, 1, *n, f) != (size_t)*n) { perror(path); exit(2); }
    fclose(f);
    return d;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s DISK.dsk [DISK.dsk...]\n", argv[0]); return 2; }
    /* entry 1 of 'dcmp' 3: LINK A6,#-$2A ; MOVEA.L 8(A6),A0 ; MOVE.L 8(A0),-$2A(A6) */
    static const uint8_t e1[] = { 0x4E,0x56,0xFF,0xD6,0x20,0x6E,0x00,0x08,0x2D,0x68,0x00,0x08,0xFF,0xD6 };
    long n0; uint8_t *d0 = load(argv[1], &n0);
    int found = 0;
    for (long i = DC_ENTRY1; i + (long)DC_LEN - DC_ENTRY1 <= n0 && !found; i++) {
        if (memcmp(d0 + i, e1, sizeof e1)) continue;
        memcpy(RAM + CODE, d0 + i - DC_ENTRY1, DC_LEN);
        dc_ram = RAM; dc_ram_size = RAMSZ;
        if (dc_hash(CODE, DC_LEN, 0) == DC_HASH) { memcpy(code_img, d0 + i - DC_ENTRY1, DC_LEN); found = 1; }
    }
    free(d0);
    if (!found) { fprintf(stderr, "'dcmp' 3 code not found in %s\n", argv[1]); return 2; }

    const int cpus[2] = { M68K_CPU_TYPE_68000, M68K_CPU_TYPE_68030 };
    long runs = 0, bad = 0, bytes = 0; unsigned long long instr = 0;
    for (int di = 1; di < argc; di++) {
        long n; uint8_t *d = load(argv[di], &n);
        for (long i = 4; i + 18 < n; i++) {
            if (!(d[i] == 0xA8 && d[i+1] == 0x9F && d[i+2] == 0x65 && d[i+3] == 0x72)) continue;
            if (d[i+6] != 9 || d[i+12] != 0 || d[i+13] != 3) continue;      /* v9, dcmp 3 */
            uint32_t rlen  = (d[i-4] << 24) | (d[i-3] << 16) | (d[i-2] << 8) | d[i-1];
            uint32_t usize = (d[i+8] << 24) | (d[i+9] << 16) | (d[i+10] << 8) | d[i+11];
            uint32_t w14   = (d[i+14] << 8) | d[i+15];
            if (rlen < 19 || i + (long)rlen > n || usize > 0x200000) continue;
            uint32_t clen = rlen - 18, d5 = usize + w14;
            if (d5 < clen) d5 = clen;
            for (int ci = 0; ci < 2; ci++) {
                memset(RAM, 0x5A, RAMSZ);
                memcpy(RAM + CODE, code_img, DC_LEN);
                memcpy(RAM + HDR, d + i, 18);
                uint32_t src = BUF + d5 - clen;           /* in place, like the glue */
                memcpy(RAM + src, d + i + 18, clen);
                m68k_write_memory_32(0, STK); m68k_write_memory_32(4, RET);
                m68k_write_memory_16(RET, 0x4E71);
                m68k_init(); m68k_set_cpu_type(cpus[ci]); m68k_pulse_reset();
                uint32_t sp = STK - 16;
                m68k_write_memory_32(sp, RET);     m68k_write_memory_32(sp + 4, HDR);
                m68k_write_memory_32(sp + 8, BUF); m68k_write_memory_32(sp + 12, src);
                m68k_write_memory_16(sp + 16, 0xBEEF);       /* cleared by entry 1 */
                for (int r = 0; r < 8; r++) m68k_set_reg(M68K_REG_D0 + r, 0x11111111u * (r + 1) ^ (uint32_t)i);
                for (int r = 0; r < 7; r++) m68k_set_reg(M68K_REG_A0 + r, 0x01010100u * (r + 1) ^ (uint32_t)i);
                m68k_set_reg(M68K_REG_SP, sp);
                m68k_set_reg(M68K_REG_SR, 0x2700 | (i & 0x1F));
                m68k_set_reg(M68K_REG_PC, CODE + DC_ENTRY1);
                memcpy(snap, RAM, RAMSZ);
                m68ki_cpu_core c0; memcpy(&c0, &m68ki_cpu, sizeof c0);
                unsigned long k = 0;
                while ((REG_PC & 0xFFFFFF) != RET && ++k < 1000000000ul) m68k_execute(1);
                instr += k;
                m68ki_cpu_core c68; memcpy(&c68, &m68ki_cpu, sizeof c68);
                memcpy(r68, RAM, RAMSZ);

                memcpy(RAM, snap, RAMSZ); memcpy(&m68ki_cpu, &c0, sizeof c0);
                RAM[CODE + DC_ENTRY1] = DC_TRAP >> 8; RAM[CODE + DC_ENTRY1 + 1] = DC_TRAP & 0xFF;
                REG_PC = CODE + DC_ENTRY1 + 2;           /* as after fetching the trap word */
                int e = !dc_trap();
                RAM[CODE + DC_ENTRY1] = DC_OP1 >> 8; RAM[CODE + DC_ENTRY1 + 1] = DC_OP1 & 0xFF;
                for (uint32_t a = 0; a < RAMSZ; a++) {
                    if (a >= STK - 0x1000 && a < sp) continue;   /* dead stack */
                    if (RAM[a] != r68[a]) {
                        if (e < 4) fprintf(stderr, "  RAM $%06X 68k=%02X native=%02X\n", a, r68[a], RAM[a]);
                        e++;
                    }
                }
                for (int r = 0; r < 16; r++)
                    if (m68ki_cpu.dar[r] != c68.dar[r]) {
                        fprintf(stderr, "  %c%d 68k=%08X native=%08X\n", r < 8 ? 'D' : 'A', r & 7,
                                c68.dar[r], m68ki_cpu.dar[r]);
                        e++;
                    }
                if ((m68ki_cpu.pc & 0xFFFFFF) != (c68.pc & 0xFFFFFF) ||
                    !!(m68ki_cpu.x_flag & 0x100) != !!(c68.x_flag & 0x100) ||
                    !!(m68ki_cpu.n_flag & 0x80) != !!(c68.n_flag & 0x80) ||
                    !m68ki_cpu.not_z_flag != !c68.not_z_flag ||
                    !!(m68ki_cpu.v_flag & 0x80) != !!(c68.v_flag & 0x80) ||
                    !!(m68ki_cpu.c_flag & 0x100) != !!(c68.c_flag & 0x100)) {
                    fprintf(stderr, "  PC/flags differ\n");
                    e++;
                }
                runs++;
                if (e) { bad++; fprintf(stderr, "MISMATCH %s offset %ld usize %u clen %u cpu %d\n",
                                        argv[di], i, usize, clen, cpus[ci]); }
                if (ci == 0) bytes += usize;
            }
        }
        free(d);
    }
    printf("dcmp-3 check: %ld runs (each resource on 68000 and 68030), %ld mismatches, "
           "%ld bytes per CPU, %llu 68k instructions\n", runs, bad, bytes, instr);
    return bad != 0;
}
