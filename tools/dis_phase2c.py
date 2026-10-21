import sys
sys.path.insert(0, r"E:\Personal\Macintosh\tools")
from dis68k import Dis68k, read_rom, fmt

rom = read_rom(r"E:\Personal\Macintosh\ROM\LC-II.ROM")
dis = Dis68k(rom)
print(fmt(dis.disassemble(0x40BA90, 0x40BC10)))
