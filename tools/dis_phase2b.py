import sys
sys.path.insert(0, r"E:\Personal\Macintosh\tools")
from dis68k import Dis68k, read_rom, fmt

rom = read_rom(r"E:\Personal\Macintosh\ROM\LC-II.ROM")
dis = Dis68k(rom)
ROM_BASE = 0x400000

pats = [
    ("spin_loop",   bytes([0x30, 0x28, 0x00, 0x10, 0x6E])),
    ("dm_entry",    bytes([0x34, 0x28, 0x00, 0x18, 0x6B])),
    ("movem_entry", bytes([0x48, 0xE7, 0x1C, 0x38, 0x34, 0x28, 0x00, 0x18])),
    ("drive_ge4",   bytes([0x0C, 0x69, 0x00, 0x04, 0x00, 0x12])),
]

for name, pat in pats:
    hits = []
    start = 0
    while True:
        i = rom.find(pat, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1
    print("pattern %s: %d hits" % (name, len(hits)))
    for i in hits[:40]:
        print("  $%06X" % (ROM_BASE + i))
