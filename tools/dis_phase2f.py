import sys
sys.path.insert(0, r"E:\Personal\Macintosh\tools")
from dis68k import Dis68k, read_rom, fmt

rom = read_rom(r"E:\Personal\Macintosh\ROM\LC-II.ROM")
dis = Dis68k(rom)
print("=== LC II boot-read helper $40160C-$401620 ===")
print(fmt(dis.disassemble(0x40160C, 0x401620)))
print("=== LC II caller of helper: find JSR to $401616 ===")
rom_bytes = read_rom(r"E:\Personal\Macintosh\ROM\LC-II.ROM")
ROM_BASE = 0x400000
target = 0x401616
hits = []
for i in range(0, len(rom_bytes) - 6, 2):
    if rom_bytes[i] == 0x4E and (rom_bytes[i+1] & 0xF0) in (0x40, 0x80):
        op = (rom_bytes[i+1] << 8) | rom_bytes[i+2]
        mode = (op >> 8) & 0x07 if False else ((rom_bytes[i+1] >> 4) & 7)
        if mode == 0x0F:
            val = (rom_bytes[i+2] << 24) | (rom_bytes[i+3] << 16) | (rom_bytes[i+4] << 8) | rom_bytes[i+5]
            if val == target:
                hits.append(ROM_BASE + i)
for h in hits[:20]:
    print("  JSR/JMP abs.L -> $401616 at $%06X" % h)
