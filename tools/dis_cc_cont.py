import sys
sys.path.insert(0, r"E:\Personal\Macintosh\tools")
from dis68k import Dis68k, read_rom, fmt

rom = read_rom(r"E:\Personal\Macintosh\ROM\Color-Classic.ROM")
dis = Dis68k(rom, rom_base=0xA00000)
print("=== continuation head bus $A10000-$A11000 (file $10000-$11000) ===")
print(fmt(dis.disassemble(0xA10000, 0xA11000)))
