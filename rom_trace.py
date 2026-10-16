import struct

with open('D:/Personal/Macintosh/ROM/Classic.ROM', 'rb') as f:
    rom = f.read()

def read_word(offset):
    return (rom[offset] << 8) | rom[offset+1]

def read_long(offset):
    return struct.unpack_from('>I', rom, offset)[0]

def dump_range(start_offset, count, label):
    print(f"\n=== {label} (ROM offset 0x{start_offset:06X}, addr 0x{0x400000+start_offset:06X}) ===")
    off = start_offset
    for _ in range(count):
        if off >= len(rom) - 1:
            break
        w = read_word(off)
        addr = 0x400000 + off
        extra = ""

        # Basic 68000 decode
        if w == 0x4E71: extra = "NOP"
        elif w == 0x4E75: extra = "RTS"
        elif w == 0x4E73: extra = "RTE"
        elif w == 0x4E76: extra = "TRAPV"
        elif w == 0x4E70: extra = "RESET"
        elif w == 0x60FE: extra = "BRA.S * (infinite loop!)"
        elif w == 0x46FC:
            imm = read_word(off+2)
            extra = f"MOVE #{imm:04X},SR"
        elif w == 0x4EF9:
            target = read_long(off+2)
            extra = f"JMP {target:08X}"
        elif w == 0x4EB9:
            target = read_long(off+2)
            extra = f"JSR {target:08X}"
        elif (w & 0xFFFF) == 0x4EFA:
            disp = read_word(off+2)
            if disp > 0x7FFF: disp -= 0x10000
            target = addr + 2 + disp
            extra = f"JMP (PC,{disp:+d}) -> {target:06X}"
        elif (w & 0xFFFF) == 0x4EBA:
            disp = read_word(off+2)
            if disp > 0x7FFF: disp -= 0x10000
            target = addr + 2 + disp
            extra = f"JSR (PC,{disp:+d}) -> {target:06X}"
        elif (w >> 8) == 0x61 and (w & 0xFF) != 0:
            disp = w & 0xFF
            if disp > 0x7F: disp -= 0x100
            target = addr + 2 + disp
            extra = f"BSR.S -> {target:06X}"
        elif w == 0x6100:
            disp = read_word(off+2)
            if disp > 0x7FFF: disp -= 0x10000
            target = addr + 2 + disp
            extra = f"BSR.W -> {target:06X}"
        elif (w >> 8) == 0x60 and (w & 0xFF) != 0:
            disp = w & 0xFF
            if disp > 0x7F: disp -= 0x100
            extra = f"BRA.S *{disp+2:+d}"
        elif w == 0x6000:
            disp = read_word(off+2)
            if disp > 0x7FFF: disp -= 0x10000
            target = addr + 2 + disp
            extra = f"BRA.W -> {target:06X}"
        elif (w & 0xF1F8) == 0x2E48:
            extra = f"MOVEA.L A{w&7},SP"
        elif (w & 0xFFF8) == 0x2E40:
            extra = f"MOVEA.L D{w&7},SP"
        elif w == 0x2E7C:
            val = read_long(off+2)
            extra = f"MOVEA.L #{val:08X},SP"
        elif (w & 0xF100) == 0x2000:
            extra = "MOVE.L ..."
        elif (w & 0xF100) == 0x3000:
            extra = "MOVE.W ..."
        elif (w & 0xFF00) == 0x0800:
            extra = "BTST #imm,..."
        elif (w & 0xFFC0) == 0x08C0:
            extra = "BSET #imm,..."
        elif (w & 0xFFC0) == 0x0880:
            extra = "BCLR #imm,..."
        elif (w & 0xFFC0) == 0x0840:
            extra = "BCHG #imm,..."
        elif (w >> 12) == 0x0:
            extra = "(immediate op)"

        print(f"  {addr:06X}: {w:04X}  {extra}")
        off += 2

# Entry point
print("=== ROM ENTRY FLOW ===")
dump_range(0x0040, 20, "Entry (after MOVE #2700,SR)")

# Jump target at 0x1BDE
print("\n")
dump_range(0x1BDE, 40, "Jump target from entry (0x401BDE)")

# Look at code around 0x04E0 area (BSR targets from entry)
# From 0x400048: 6100 04E0 = BSR.W -> 0x40052A
print("\n")
dump_range(0x052A, 20, "BSR target 0x40052A (from entry)")

# Check 0x400424 area (first VIA ref found)
print("\n")
dump_range(0x0420, 20, "VIA ref area 0x400424")

# Check the PC values we observed
for pc in [0x4026DA, 0x40222E, 0x402334, 0x402302, 0x40244C]:
    dump_range(pc - 0x400000 - 0x10, 20, f"Around PC=0x{pc:06X}")
    print()
