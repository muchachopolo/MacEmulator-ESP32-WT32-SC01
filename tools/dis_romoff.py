import sys
sys.path.insert(0, r"E:\Personal\Macintosh\tools")
from dis68k import Dis68k, fmt

rom = open(r"E:\Personal\Macintosh\ROM\Color-Classic.ROM", "rb").read()
off = int(sys.argv[1], 16)
n = int(sys.argv[2]) if len(sys.argv) > 2 else 48
print("file bytes @%06X:" % off, " ".join("%02X" % b for b in rom[off:off+n]))
data = rom[off:off+n]
dis = Dis68k(data, rom_base=0)
print(fmt(dis.disassemble(0, len(data))))
