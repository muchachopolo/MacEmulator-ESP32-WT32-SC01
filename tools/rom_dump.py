import sys
sys.path.insert(0, r"E:\Personal\Macintosh\tools")
from dis68k import Dis68k, read_rom, fmt

for name, offs in [("LC-II.ROM", [0x60000, 0x6FF00]),
                   ("Color-Classic.ROM", [0x60000, 0x6FF00, 0x80000, 0x80020, 0xA0000, 0xA0020, 0xFFF00])]:
    rom = read_rom(r"E:\Personal\Macintosh\ROM" + "\\" + name)
    print(f"=== {name} ({len(rom)//1024} KB) ===")
    for off in offs:
        b = rom[off:off+32]
        print(f"@{off:06X}: " + " ".join(f"{x:02X}" for x in b))
    dis = Dis68k(rom)
    for off in offs:
        print(f"--- dis file@{off:06X} ---")
        print(fmt(dis.disassemble(0x400000 + off, 0x400000 + off + 0x40)))
