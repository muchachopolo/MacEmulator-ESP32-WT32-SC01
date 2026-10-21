import sys
sys.path.insert(0, r"E:\Personal\Macintosh\tools")
from dis68k import Dis68k, read_rom, fmt

regions = [
    ("Classic.ROM", 0x400000, 0x400080, "Classic vectors + first-stage boot"),
    ("Classic.ROM", 0x440000, 0x440040, "Classic ROM offset $40000 (JMP $140000 target)"),
    ("LC-II.ROM", 0x400000, 0x400080, "LC II vectors + first-stage boot"),
    ("LC-II.ROM", 0x440000, 0x440040, "LC II ROM offset $40000"),
]
for name, start, end, desc in regions:
    rom = read_rom(r"E:\Personal\Macintosh\ROM" + "\\" + name)
    dis = Dis68k(rom)
    print("=== %s $%06X-$%06X (%s) ===" % (name, start, end, desc))
    print(fmt(dis.disassemble(start, end)))
