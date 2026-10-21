import sys
sys.path.insert(0, r"E:\Personal\Macintosh\tools")
from dis68k import Dis68k, read_rom, fmt

rom = read_rom(r"E:\Personal\Macintosh\ROM\Classic.ROM")
dis = Dis68k(rom)
print("=== Classic _Read trap handler $400E00-$400E14 ===")
print(fmt(dis.disassemble(0x400E00, 0x400E14)))
print("=== Classic .SONY Prime + entry $434B70-$434BC0 ===")
print(fmt(dis.disassemble(0x434B70, 0x434BC0)))
print("=== Classic DM driver-request routine $402F36-$402F60 ===")
print(fmt(dis.disassemble(0x402F36, 0x402F60)))
