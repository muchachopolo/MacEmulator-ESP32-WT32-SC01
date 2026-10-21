import sys
sys.path.insert(0, r"E:\Personal\Macintosh\tools")
from dis68k import Dis68k, fmt

hexstr = sys.argv[1]
base = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
data = bytes(int(hexstr[i:i+2], 16) for i in range(0, len(hexstr.replace(' ', '')), 2))
dis = Dis68k(data, rom_base=0)
print(fmt(dis.disassemble(0, len(data))))
