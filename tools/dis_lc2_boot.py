import sys
sys.path.insert(0, r"E:\Personal\Macintosh\tools")
from dis68k import Dis68k, read_rom, fmt

rom = read_rom(r"E:\Personal\Macintosh\ROM\LC-II.ROM")
dis = Dis68k(rom)
print("=== LC-II.ROM $40002A-$400120 (boot entry, JSR $600000 context) ===")
print(fmt(dis.disassemble(0x40002A, 0x400120)))
print("=== LC-II.ROM $45FF80-$460060 (end of content / padding start) ===")
print(fmt(dis.disassemble(0x45FF80, 0x460060)))
