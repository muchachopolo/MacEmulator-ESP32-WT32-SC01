import sys
sys.path.insert(0, r"E:\Personal\Macintosh\tools")

for n in ["Classic.ROM", "LC-II.ROM"]:
    d = open(r"E:\Personal\Macintosh\ROM" + "\\" + n, "rb").read()
    print(n, "size", len(d))
    print("  $00-$0F :", " ".join("%02X" % b for b in d[0x00:0x10]))
    print("  $20-$3F :", " ".join("%02X" % b for b in d[0x20:0x40]))
    print("  $40-$5F :", " ".join("%02X" % b for b in d[0x40:0x60]))
