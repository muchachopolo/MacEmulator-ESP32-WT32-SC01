import sys
sys.path.insert(0, r"E:\Personal\Macintosh\tools")
from dis68k import Dis68k, read_rom, fmt

rom = read_rom(r"E:\Personal\Macintosh\ROM\Classic.ROM")
dis = Dis68k(rom)

regions = [
    (0x419B00, 0x419B20, "GetRes FONT call site (RES2 #1 here)"),
    (0x41A000, 0x41A020, "GetRes PACK call site (RES2 #0 here)"),
    (0x419730, 0x419750, "DSERR-CHG PC $419742 (F918 handler)"),
    (0x419140, 0x419160, "_InitFonts entry $419152"),
    (0x419AD8, 0x419B00, "GetRes NFNT site $419AEA"),
    (0x419D48, 0x419D60, "GetRes FOND site $419D56"),
    (0x41A840, 0x41A860, "SCSI polling loop PC $41A84A"),
    (0x400708, 0x400720, "DSERR-CHG PC $400714"),
    (0x400760, 0x400770, "UNIMP handler $400768"),
]

for start, end, desc in regions:
    print(f"\n{'='*76}")
    print(f"  {desc}")
    print(f"  ${start:06X} - ${end:06X}")
    print("=" * 76)
    print(fmt(dis.disassemble(start, end)))
