import sys
sys.path.insert(0, r"E:\Personal\Macintosh\tools")
from dis68k import Dis68k, read_rom, fmt

rom = read_rom(r"E:\Personal\Macintosh\ROM\Color-Classic.ROM")
dis = Dis68k(rom, rom_base=0xA00000)
print("=== boot entry bus $A00000-$A00100 (file $0-$100) ===")
print(fmt(dis.disassemble(0xA00000, 0xA00100)))
dis2 = Dis68k(rom, rom_base=0x5F0000)
print("=== Start Manager stub bus $5FFFD2-$61002E (file $0FFD2-$11000) ===")
print(fmt(dis2.disassemble(0x5FFFD2, 0x61002E)))
