#!/usr/bin/env python3
"""
Motorola 68000 disassembler for Mac Classic ROM analysis.
Handles the common instructions found in the .SONY driver / SWIM code.
Properly prioritizes opcode decoding to avoid mismatches.
"""

import struct
import sys

ROM_BASE = 0x400000

def read_rom(path):
    with open(path, 'rb') as f:
        return f.read()


class Dis68k:
    def __init__(self, rom_data, rom_base=ROM_BASE):
        self.rom = rom_data
        self.base = rom_base
        self.pc = 0

    def r16(self, addr):
        off = addr - self.base
        if off < 0 or off + 2 > len(self.rom):
            return 0
        return struct.unpack('>H', self.rom[off:off+2])[0]

    def r32(self, addr):
        off = addr - self.base
        if off < 0 or off + 4 > len(self.rom):
            return 0
        return struct.unpack('>I', self.rom[off:off+4])[0]

    def s16(self, v):
        return v - 0x10000 if v >= 0x8000 else v

    def s8(self, v):
        return v - 0x100 if v >= 0x80 else v

    def s32(self, v):
        return v - 0x100000000 if v >= 0x80000000 else v

    def sz_str(self, bits):
        """2-bit size field: 00=B, 01=W, 10=L"""
        return {0: 'B', 1: 'W', 2: 'L'}.get(bits, '?')

    def ea(self, mode, reg, sz='W'):
        """Decode effective address. Returns string. Advances self.pc for extension words."""
        if mode == 0:
            return f'D{reg}'
        if mode == 1:
            return f'A{reg}'
        if mode == 2:
            return f'(A{reg})'
        if mode == 3:
            return f'(A{reg})+'
        if mode == 4:
            return f'-(A{reg})'
        if mode == 5:
            d = self.s16(self.r16(self.pc)); self.pc += 2
            if d == 0: return f'(A{reg})'
            if d < 0: return f'-${-d:X}(A{reg})'
            return f'${d:X}(A{reg})'
        if mode == 6:
            ext = self.r16(self.pc); self.pc += 2
            d = self.s8(ext & 0xFF)
            xr = (ext >> 12) & 7
            xt = 'A' if ext & 0x8000 else 'D'
            xs = '.L' if ext & 0x0800 else '.W'
            if d == 0: return f'(A{reg},{xt}{xr}{xs})'
            if d < 0: return f'-${-d:X}(A{reg},{xt}{xr}{xs})'
            return f'${d:X}(A{reg},{xt}{xr}{xs})'
        if mode == 7:
            if reg == 0:  # xxx.W
                a = self.s16(self.r16(self.pc)); self.pc += 2
                if a < 0: return f'(${a & 0xFFFFFFFF:08X}).W'
                return f'(${a:04X}).W'
            if reg == 1:  # xxx.L
                a = self.r32(self.pc); self.pc += 4
                return f'(${a:08X}).L'
            if reg == 2:  # d16(PC)
                base = self.pc
                d = self.s16(self.r16(self.pc)); self.pc += 2
                return f'${base+d:X}(PC)'
            if reg == 3:  # d8(PC,Xn)
                base = self.pc
                ext = self.r16(self.pc); self.pc += 2
                d = self.s8(ext & 0xFF)
                xr = (ext >> 12) & 7
                xt = 'A' if ext & 0x8000 else 'D'
                xs = '.L' if ext & 0x0800 else '.W'
                return f'${base+d:X}(PC,{xt}{xr}{xs})'
            if reg == 4:  # #imm
                if sz == 'B':
                    v = self.r16(self.pc) & 0xFF; self.pc += 2
                    return f'#${v:02X}'
                if sz == 'W':
                    v = self.r16(self.pc); self.pc += 2
                    return f'#${v:04X}'
                if sz == 'L':
                    v = self.r32(self.pc); self.pc += 4
                    return f'#${v:08X}'
        return f'???(m{mode}r{reg})'

    def imm_str(self, sz):
        """Read an immediate value and return string."""
        if sz == 'B':
            v = self.r16(self.pc) & 0xFF; self.pc += 2
            return f'#${v:02X}'
        if sz == 'W':
            v = self.r16(self.pc); self.pc += 2
            return f'#${v:04X}'
        v = self.r32(self.pc); self.pc += 4
        return f'#${v:08X}'

    def reglist_str(self, mask, reverse=False):
        """Format MOVEM register list."""
        names = []
        for i in range(16):
            b = (15 - i) if reverse else i
            if mask & (1 << b):
                names.append(f'D{i}' if i < 8 else f'A{i-8}')
        return '/'.join(names) if names else '(none)'

    def decode(self):
        """Decode one instruction at self.pc. Returns (start_addr, raw_bytes, mnemonic)."""
        start = self.pc
        w = self.r16(self.pc)
        self.pc += 2

        def done(mn):
            off = start - self.base
            return start, self.rom[off:off+(self.pc-start)], mn

        top4 = (w >> 12) & 0xF
        mode6 = (w >> 3) & 7
        reg0 = w & 7
        reg9 = (w >> 9) & 7
        sz_bits = (w >> 6) & 3

        # ================================================================
        # Group 0: ORI/ANDI/SUBI/ADDI/EORI/CMPI/BTST/BCHG/BCLR/BSET
        # ================================================================
        if top4 == 0:
            sub = (w >> 8) & 0xF

            # ORI (0000_0000)
            if sub == 0x00 and sz_bits != 3:
                sz = self.sz_str(sz_bits)
                imm = self.imm_str(sz)
                if mode6 == 7 and reg0 == 4:
                    return done(f'ORI {imm},{"SR" if sz_bits else "CCR"}')
                return done(f'ORI.{sz} {imm},{self.ea(mode6, reg0, sz)}')

            # ANDI (0000_0010)
            if sub == 0x02 and sz_bits != 3:
                sz = self.sz_str(sz_bits)
                imm = self.imm_str(sz)
                if mode6 == 7 and reg0 == 4:
                    return done(f'ANDI {imm},{"SR" if sz_bits else "CCR"}')
                return done(f'ANDI.{sz} {imm},{self.ea(mode6, reg0, sz)}')

            # SUBI (0000_0100)
            if sub == 0x04 and sz_bits != 3:
                sz = self.sz_str(sz_bits)
                imm = self.imm_str(sz)
                return done(f'SUBI.{sz} {imm},{self.ea(mode6, reg0, sz)}')

            # ADDI (0000_0110)
            if sub == 0x06 and sz_bits != 3:
                sz = self.sz_str(sz_bits)
                imm = self.imm_str(sz)
                return done(f'ADDI.{sz} {imm},{self.ea(mode6, reg0, sz)}')

            # EORI (0000_1010)
            if sub == 0x0A and sz_bits != 3:
                sz = self.sz_str(sz_bits)
                imm = self.imm_str(sz)
                if mode6 == 7 and reg0 == 4:
                    return done(f'EORI {imm},{"SR" if sz_bits else "CCR"}')
                return done(f'EORI.{sz} {imm},{self.ea(mode6, reg0, sz)}')

            # CMPI (0000_1100)
            if sub == 0x0C and sz_bits != 3:
                sz = self.sz_str(sz_bits)
                imm = self.imm_str(sz)
                return done(f'CMPI.{sz} {imm},{self.ea(mode6, reg0, sz)}')

            # BTST/BCHG/BCLR/BSET (immediate bit#) (0000_1000_xx)
            if sub == 0x08:
                ops = ['BTST', 'BCHG', 'BCLR', 'BSET']
                op = ops[sz_bits]
                bn = self.r16(self.pc) & 0x1F; self.pc += 2
                sz = 'L' if mode6 == 0 else 'B'
                return done(f'{op} #{bn},{self.ea(mode6, reg0, sz)}')

            # BTST/BCHG/BCLR/BSET (register bit#) -- top byte 0000_rrr1_xx
            if (w & 0x0100) and not (w & 0x00C0 == 0x00C0 and mode6 >= 2):
                ops = ['BTST', 'BCHG', 'BCLR', 'BSET']
                op = ops[sz_bits]
                sz = 'L' if mode6 == 0 else 'B'
                return done(f'{op} D{reg9},{self.ea(mode6, reg0, sz)}')

            # MOVEP (0000_rrr1_0x_001_rrr) -- reg to/from d(An)
            if (w & 0x0138) == 0x0108:
                dreg = reg9
                areg = reg0
                disp = self.s16(self.r16(self.pc)); self.pc += 2
                if w & 0x0080:  # long
                    if w & 0x0040:
                        return done(f'MOVEP.L D{dreg},${disp & 0xFFFF:04X}(A{areg})')
                    else:
                        return done(f'MOVEP.L ${disp & 0xFFFF:04X}(A{areg}),D{dreg}')
                else:
                    if w & 0x0040:
                        return done(f'MOVEP.W D{dreg},${disp & 0xFFFF:04X}(A{areg})')
                    else:
                        return done(f'MOVEP.W ${disp & 0xFFFF:04X}(A{areg}),D{dreg}')

        # ================================================================
        # Group 1,2,3: MOVE / MOVEA
        # ================================================================
        if top4 in (1, 2, 3):
            sz_map = {1: 'B', 3: 'W', 2: 'L'}
            sz = sz_map[top4]
            dst_mode = (w >> 6) & 7
            dst_reg = reg9

            src = self.ea(mode6, reg0, sz)

            if dst_mode == 1:
                # MOVEA
                dst = f'A{dst_reg}'
                return done(f'MOVEA.{sz} {src},{dst}')
            else:
                dst = self.ea(dst_mode, dst_reg, sz)
                return done(f'MOVE.{sz} {src},{dst}')

        # ================================================================
        # Group 4: Misc (CLR, NEG, NOT, TST, MOVE SR/CCR/USP, LEA, PEA,
        #          JMP, JSR, RTS, RTE, TRAP, LINK, UNLK, MOVEM, SWAP, EXT, etc.)
        # ================================================================
        if top4 == 4:
            # MOVE from SR: 0100_0000_11_mmm_rrr
            if (w & 0xFFC0) == 0x40C0:
                return done(f'MOVE.W SR,{self.ea(mode6, reg0, "W")}')

            # CLR: 0100_0010_ss_mmm_rrr
            if (w & 0xFF00) == 0x4200 and sz_bits != 3:
                sz = self.sz_str(sz_bits)
                return done(f'CLR.{sz} {self.ea(mode6, reg0, sz)}')

            # MOVE to CCR: 0100_0100_11_mmm_rrr
            if (w & 0xFFC0) == 0x44C0:
                return done(f'MOVE.W {self.ea(mode6, reg0, "W")},CCR')

            # NEG: 0100_0100_ss_mmm_rrr
            if (w & 0xFF00) == 0x4400 and sz_bits != 3:
                sz = self.sz_str(sz_bits)
                return done(f'NEG.{sz} {self.ea(mode6, reg0, sz)}')

            # MOVE to SR: 0100_0110_11_mmm_rrr
            if (w & 0xFFC0) == 0x46C0:
                return done(f'MOVE.W {self.ea(mode6, reg0, "W")},SR')

            # NOT: 0100_0110_ss_mmm_rrr
            if (w & 0xFF00) == 0x4600 and sz_bits != 3:
                sz = self.sz_str(sz_bits)
                return done(f'NOT.{sz} {self.ea(mode6, reg0, sz)}')

            # NBCD: 0100_1000_00_mmm_rrr
            if (w & 0xFFC0) == 0x4800:
                return done(f'NBCD {self.ea(mode6, reg0, "B")}')

            # SWAP: 0100_1000_01_000_rrr
            if (w & 0xFFF8) == 0x4840:
                return done(f'SWAP D{reg0}')

            # PEA: 0100_1000_01_mmm_rrr (mode != 000)
            if (w & 0xFFC0) == 0x4840 and mode6 != 0:
                return done(f'PEA {self.ea(mode6, reg0, "L")}')

            # EXT.W: 0100_1000_10_000_rrr
            if (w & 0xFFF8) == 0x4880:
                return done(f'EXT.W D{reg0}')

            # EXT.L: 0100_1000_11_000_rrr
            if (w & 0xFFF8) == 0x48C0:
                return done(f'EXT.L D{reg0}')

            # MOVEM reg-to-mem: 0100_1000_1s_mmm_rrr
            if (w & 0xFB80) == 0x4880 and mode6 != 0:
                sz = 'L' if w & 0x0040 else 'W'
                mask = self.r16(self.pc); self.pc += 2
                rl = self.reglist_str(mask, reverse=(mode6 == 4))
                return done(f'MOVEM.{sz} {rl},{self.ea(mode6, reg0, sz)}')

            # TST: 0100_1010_ss_mmm_rrr
            if (w & 0xFF00) == 0x4A00 and sz_bits != 3:
                sz = self.sz_str(sz_bits)
                return done(f'TST.{sz} {self.ea(mode6, reg0, sz)}')

            # TAS: 0100_1010_11_mmm_rrr
            if (w & 0xFFC0) == 0x4AC0:
                return done(f'TAS {self.ea(mode6, reg0, "B")}')

            # ILLEGAL: 0100_1010_1111_1100
            if w == 0x4AFC:
                return done('ILLEGAL')

            # MOVEM mem-to-reg: 0100_1100_1s_mmm_rrr
            if (w & 0xFB80) == 0x4C80:
                sz = 'L' if w & 0x0040 else 'W'
                mask = self.r16(self.pc); self.pc += 2
                ea_s = self.ea(mode6, reg0, sz)
                rl = self.reglist_str(mask)
                return done(f'MOVEM.{sz} {ea_s},{rl}')

            # TRAP: 0100_1110_0100_vvvv
            if (w & 0xFFF0) == 0x4E40:
                return done(f'TRAP #{w & 0xF}')

            # LINK: 0100_1110_0101_0rrr
            if (w & 0xFFF8) == 0x4E50:
                d = self.s16(self.r16(self.pc)); self.pc += 2
                return done(f'LINK A{reg0},#${d & 0xFFFF:04X}')

            # UNLK: 0100_1110_0101_1rrr
            if (w & 0xFFF8) == 0x4E58:
                return done(f'UNLK A{reg0}')

            # MOVE An,USP / MOVE USP,An
            if (w & 0xFFF0) == 0x4E60:
                if w & 8:
                    return done(f'MOVE.L USP,A{reg0}')
                return done(f'MOVE.L A{w & 7},USP')

            # RESET
            if w == 0x4E70: return done('RESET')
            # NOP
            if w == 0x4E71: return done('NOP')
            # STOP
            if w == 0x4E72:
                imm = self.r16(self.pc); self.pc += 2
                return done(f'STOP #${imm:04X}')
            # RTE
            if w == 0x4E73: return done('RTE')
            # RTS
            if w == 0x4E75: return done('RTS')
            # TRAPV
            if w == 0x4E76: return done('TRAPV')
            # RTR
            if w == 0x4E77: return done('RTR')

            # JSR: 0100_1110_10_mmm_rrr
            if (w & 0xFFC0) == 0x4E80:
                return done(f'JSR {self.ea(mode6, reg0, "L")}')

            # JMP: 0100_1110_11_mmm_rrr
            if (w & 0xFFC0) == 0x4EC0:
                return done(f'JMP {self.ea(mode6, reg0, "L")}')

            # LEA: 0100_rrr1_11_mmm_rrr
            if (w & 0xF1C0) == 0x41C0:
                return done(f'LEA {self.ea(mode6, reg0, "L")},A{reg9}')

            # CHK: 0100_rrr1_10_mmm_rrr
            if (w & 0xF1C0) == 0x4180:
                return done(f'CHK {self.ea(mode6, reg0, "W")},D{reg9}')

            # NEGX: 0100_0000_ss_mmm_rrr
            if (w & 0xFF00) == 0x4000 and sz_bits != 3:
                sz = self.sz_str(sz_bits)
                return done(f'NEGX.{sz} {self.ea(mode6, reg0, sz)}')

        # ================================================================
        # Group 5: ADDQ/SUBQ/Scc/DBcc
        # ================================================================
        if top4 == 5:
            # Scc: 0101_cccc_11_mmm_rrr (but NOT mode=001 which is DBcc)
            if sz_bits == 3 and mode6 != 1:
                conds = ['T','F','HI','LS','CC','CS','NE','EQ',
                         'VC','VS','PL','MI','GE','LT','GT','LE']
                cc = conds[(w >> 8) & 0xF]
                return done(f'S{cc} {self.ea(mode6, reg0, "B")}')

            # DBcc: 0101_cccc_11_001_rrr
            if sz_bits == 3 and mode6 == 1:
                conds = ['T','F','HI','LS','CC','CS','NE','EQ',
                         'VC','VS','PL','MI','GE','LT','GT','LE']
                cc = conds[(w >> 8) & 0xF]
                base = self.pc
                d = self.s16(self.r16(self.pc)); self.pc += 2
                target = base + d
                return done(f'DB{cc} D{reg0},${target:X}')

            # ADDQ: 0101_ddd0_ss_mmm_rrr
            if not (w & 0x0100) and sz_bits != 3:
                data = reg9 if reg9 else 8
                sz = self.sz_str(sz_bits)
                return done(f'ADDQ.{sz} #{data},{self.ea(mode6, reg0, sz)}')

            # SUBQ: 0101_ddd1_ss_mmm_rrr
            if (w & 0x0100) and sz_bits != 3:
                data = reg9 if reg9 else 8
                sz = self.sz_str(sz_bits)
                return done(f'SUBQ.{sz} #{data},{self.ea(mode6, reg0, sz)}')

        # ================================================================
        # Group 6: Bcc/BRA/BSR
        # ================================================================
        if top4 == 6:
            conds = ['RA','SR','HI','LS','CC','CS','NE','EQ',
                     'VC','VS','PL','MI','GE','LT','GT','LE']
            cc = conds[(w >> 8) & 0xF]
            disp8 = w & 0xFF

            if disp8 == 0:
                d = self.s16(self.r16(self.pc)); self.pc += 2
                target = start + 2 + d
            elif disp8 == 0xFF:
                d = self.s32(self.r32(self.pc)); self.pc += 4
                target = start + 2 + d
            else:
                target = start + 2 + self.s8(disp8)

            if cc == 'RA': return done(f'BRA ${target:X}')
            if cc == 'SR': return done(f'BSR ${target:X}')
            return done(f'B{cc} ${target:X}')

        # ================================================================
        # Group 7: MOVEQ
        # ================================================================
        if top4 == 7:
            data = self.s8(w & 0xFF)
            if data < 0:
                return done(f'MOVEQ #-${-data:X},D{reg9}')
            return done(f'MOVEQ #{data},D{reg9}')

        # ================================================================
        # Group 8: OR/DIVU/DIVS/SBCD
        # ================================================================
        if top4 == 8:
            opmode = (w >> 6) & 7
            if opmode == 3:
                return done(f'DIVU.W {self.ea(mode6, reg0, "W")},D{reg9}')
            if opmode == 7:
                return done(f'DIVS.W {self.ea(mode6, reg0, "W")},D{reg9}')
            if opmode < 3:
                sz = self.sz_str(opmode)
                return done(f'OR.{sz} {self.ea(mode6, reg0, sz)},D{reg9}')
            if opmode >= 4:
                sz = self.sz_str(opmode - 4)
                return done(f'OR.{sz} D{reg9},{self.ea(mode6, reg0, sz)}')

        # ================================================================
        # Group 9: SUB/SUBA/SUBX
        # ================================================================
        if top4 == 9:
            opmode = (w >> 6) & 7
            if opmode == 3:
                return done(f'SUBA.W {self.ea(mode6, reg0, "W")},A{reg9}')
            if opmode == 7:
                return done(f'SUBA.L {self.ea(mode6, reg0, "L")},A{reg9}')
            if opmode < 3:
                sz = self.sz_str(opmode)
                return done(f'SUB.{sz} {self.ea(mode6, reg0, sz)},D{reg9}')
            # SUBX or SUB Dn,<ea>
            if opmode >= 4:
                if mode6 == 0:
                    sz = self.sz_str(opmode - 4)
                    return done(f'SUBX.{sz} D{reg0},D{reg9}')
                if mode6 == 1:
                    sz = self.sz_str(opmode - 4)
                    return done(f'SUBX.{sz} -(A{reg0}),-(A{reg9})')
                sz = self.sz_str(opmode - 4)
                return done(f'SUB.{sz} D{reg9},{self.ea(mode6, reg0, sz)}')

        # ================================================================
        # Group A: A-line traps (Mac Toolbox)
        # ================================================================
        if top4 == 0xA:
            return done(f'_Trap ${w:04X}')

        # ================================================================
        # Group B: CMP/CMPA/CMPM/EOR
        # ================================================================
        if top4 == 0xB:
            opmode = (w >> 6) & 7
            if opmode == 3:
                return done(f'CMPA.W {self.ea(mode6, reg0, "W")},A{reg9}')
            if opmode == 7:
                return done(f'CMPA.L {self.ea(mode6, reg0, "L")},A{reg9}')
            if opmode < 3:
                sz = self.sz_str(opmode)
                return done(f'CMP.{sz} {self.ea(mode6, reg0, sz)},D{reg9}')
            # EOR or CMPM
            if opmode >= 4:
                if mode6 == 1:  # CMPM
                    sz = self.sz_str(opmode - 4)
                    return done(f'CMPM.{sz} (A{reg0})+,(A{reg9})+')
                sz = self.sz_str(opmode - 4)
                return done(f'EOR.{sz} D{reg9},{self.ea(mode6, reg0, sz)}')

        # ================================================================
        # Group C: AND/MULU/MULS/EXG/ABCD
        # ================================================================
        if top4 == 0xC:
            opmode = (w >> 6) & 7
            if opmode == 3:
                return done(f'MULU.W {self.ea(mode6, reg0, "W")},D{reg9}')
            if opmode == 7:
                return done(f'MULS.W {self.ea(mode6, reg0, "W")},D{reg9}')
            # EXG
            if opmode == 5 and mode6 == 0:
                return done(f'EXG D{reg9},D{reg0}')
            if opmode == 5 and mode6 == 1:
                return done(f'EXG A{reg9},A{reg0}')
            if opmode == 6 and mode6 == 1:
                return done(f'EXG D{reg9},A{reg0}')
            if opmode < 3:
                sz = self.sz_str(opmode)
                return done(f'AND.{sz} {self.ea(mode6, reg0, sz)},D{reg9}')
            if opmode >= 4:
                sz = self.sz_str(opmode - 4)
                return done(f'AND.{sz} D{reg9},{self.ea(mode6, reg0, sz)}')

        # ================================================================
        # Group D: ADD/ADDA/ADDX
        # ================================================================
        if top4 == 0xD:
            opmode = (w >> 6) & 7
            if opmode == 3:
                return done(f'ADDA.W {self.ea(mode6, reg0, "W")},A{reg9}')
            if opmode == 7:
                return done(f'ADDA.L {self.ea(mode6, reg0, "L")},A{reg9}')
            if opmode < 3:
                sz = self.sz_str(opmode)
                return done(f'ADD.{sz} {self.ea(mode6, reg0, sz)},D{reg9}')
            if opmode >= 4:
                if mode6 == 0:
                    sz = self.sz_str(opmode - 4)
                    return done(f'ADDX.{sz} D{reg0},D{reg9}')
                if mode6 == 1:
                    sz = self.sz_str(opmode - 4)
                    return done(f'ADDX.{sz} -(A{reg0}),-(A{reg9})')
                sz = self.sz_str(opmode - 4)
                return done(f'ADD.{sz} D{reg9},{self.ea(mode6, reg0, sz)}')

        # ================================================================
        # Group E: Shifts and Rotates
        # ================================================================
        if top4 == 0xE:
            if sz_bits == 3:
                # Memory shift: 1 bit, word only
                dr = (w >> 8) & 1
                stype = (w >> 9) & 3
                ops_r = ['ASR', 'LSR', 'ROXR', 'ROR']
                ops_l = ['ASL', 'LSL', 'ROXL', 'ROL']
                op = ops_l[stype] if dr else ops_r[stype]
                return done(f'{op}.W {self.ea(mode6, reg0, "W")}')
            else:
                # Register shift
                dr = (w >> 8) & 1
                sz = self.sz_str(sz_bits)
                ir = (w >> 5) & 1
                stype = (w >> 3) & 3
                ops_r = ['ASR', 'LSR', 'ROXR', 'ROR']
                ops_l = ['ASL', 'LSL', 'ROXL', 'ROL']
                op = ops_l[stype] if dr else ops_r[stype]
                if ir:
                    return done(f'{op}.{sz} D{reg9},D{reg0}')
                cnt = reg9 if reg9 else 8
                return done(f'{op}.{sz} #{cnt},D{reg0}')

        # ================================================================
        # Group F: F-line (coprocessor / unimplemented)
        # ================================================================
        if top4 == 0xF:
            return done(f'DC.W ${w:04X}  ; F-line')

        return done(f'DC.W ${w:04X}')

    def disassemble(self, start_addr, end_addr):
        self.pc = start_addr
        results = []
        while self.pc < end_addr:
            r = self.decode()
            results.append(r)
        return results


def fmt(results):
    lines = []
    for addr, raw, mn in results:
        h = raw.hex().upper()
        hg = ' '.join(h[i:i+4] for i in range(0, len(h), 4))
        lines.append(f'  ${addr:06X}:  {hg:<28s} {mn}')
    return '\n'.join(lines)


def main():
    rom = read_rom(r'D:\Personal\Macintosh\ROM\Classic.ROM')
    dis = Dis68k(rom)

    regions = [
        (0x435196, 0x4351F0, "ISM/IWM branch point and sector-read entry"),
        (0x435534, 0x435580, "Sector-read sub called from SECTOR-VERIFY"),
        (0x435CF0, 0x435D70, "HANDSHAKE read and MODE0 writes"),
        (0x43580C, 0x435870, "ISM read path for drives >= 4"),
    ]

    for start, end, desc in regions:
        print(f'\n{"="*76}')
        print(f'  {desc}')
        print(f'  ${start:06X} - ${end:06X}  (file offset ${start-ROM_BASE:05X} - ${end-ROM_BASE:05X})')
        print(f'{"="*76}')
        results = dis.disassemble(start, end)
        print(fmt(results))
        print()


if __name__ == '__main__':
    main()
