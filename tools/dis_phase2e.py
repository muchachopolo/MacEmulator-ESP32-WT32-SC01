import sys
sys.path.insert(0, r"E:\Personal\Macintosh\tools")
from dis68k import Dis68k, read_rom, fmt

rom = read_rom(r"E:\Personal\Macintosh\ROM\LC-II.ROM")
ROM_BASE = 0x400000

pats = [
    ("boot_sig_cmpi", bytes([0x0C, 0x56, 0x4C, 0x4B])),
    ("trap_a002",     bytes([0xA0, 0x02, 0x66])),
    ("jmp_dm_read",   bytes([0x4E, 0xFA, 0x00, 0x00])),
]
for name, pat in pats:
    hits = []
    start = 0
    while True:
        i = rom.find(pat, start)
        if i < 0:
            break
        if i % 2 == 0:
            hits.append(i)
        start = i + 1
    print("pattern %s: %d word-aligned hits" % (name, len(hits)))
    for i in hits[:30]:
        print("  $%06X" % (ROM_BASE + i))
