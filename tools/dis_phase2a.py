import sys
sys.path.insert(0, r"E:\Personal\Macintosh\tools")
from dis68k import Dis68k, read_rom, fmt

rom = read_rom(r"E:\Personal\Macintosh\ROM\Classic.ROM")
dis = Dis68k(rom)

regions = [
    (0x403140, 0x403200, "Classic DM _Read/_Write/_Ctl/_Stat entries + spin loop"),
    (0x4357C0, 0x435870, "Classic SEC-RD / .SONY request-complete area"),
]
for start, end, desc in regions:
    print("=== %s ===" % desc)
    print(fmt(dis.disassemble(start, end)))
