import sys
sys.path.insert(0, r"E:\Personal\Macintosh\tools")
from dis68k import read_rom

rom = read_rom(r"E:\Personal\Macintosh\ROM\LC-II.ROM")
n = len(rom)

ranges = [(0x600000, 0x6FFFFF), (0xA00000, 0xAFFFFF), (0x40A00000, 0x40AFFFFF)]
for i in range(n - 5):
    op = (rom[i] << 8) | rom[i + 1]
    if op in (0x4EF9, 0x4EFA):
        tgt = int.from_bytes(rom[i + 2:i + 6], "big")
        for lo, hi in ranges:
            if lo <= tgt <= hi:
                kind = "JSR" if op == 0x4EFA else "JMP"
                print(f"@{i:06X}: {kind}.L ${tgt:08X}")
                break

for val in (0x006FF00, 0x00AFF00, 0x40A6FF00, 0x00600000, 0x00A00000, 0x40A00000):
    pat = val.to_bytes(4, "big")
    hits = [i for i in range(n) if rom.startswith(pat, i)]
    print(f"long ${val:08X}: {len(hits)} hits " + ", ".join(f"${i:X}" for i in hits[:20]))
