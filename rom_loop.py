import struct

with open('D:/Personal/Macintosh/ROM/Classic.ROM', 'rb') as f:
    rom = f.read()

# Dump raw hex + word decode for ROM around 0x2420-0x2460
# (PC stuck at 0x402438 = ROM offset 0x2438)
print("ROM bytes at offsets 0x2420-0x2470:")
for off in range(0x2420, 0x2470, 2):
    w = (rom[off] << 8) | rom[off+1]
    addr = 0x400000 + off
    print(f"  {addr:06X}: {rom[off]:02X} {rom[off+1]:02X}  ({w:04X})")

print()
# Also look at SCC write addresses used by the ROM
# Search for 0xBFFFF references (SCC write base)
print("SCC write references in ROM:")
for i in range(0, len(rom)-4, 2):
    long_val = struct.unpack_from('>I', rom, i)[0]
    if (long_val & 0xFFFFF0) == 0x00BFFFF0:
        print(f"  ROM offset 0x{i:06X}: ref to 0x{long_val:08X}")
        # Show context: previous 4 words
        for j in range(-8, 12, 2):
            idx = i + j
            if 0 <= idx < len(rom)-1:
                cw = (rom[idx] << 8) | rom[idx+1]
                mark = " <--" if j == 0 else ""
                print(f"    {0x400000+idx:06X}: {cw:04X}{mark}")
        print()
    if i > 0x2500 and i < 0x20000:
        continue  # skip middle of ROM for speed

# Also look at SCC read refs
print("\nSCC read references in ROM (first 10):")
count = 0
for i in range(0, len(rom)-4, 2):
    long_val = struct.unpack_from('>I', rom, i)[0]
    if (long_val & 0xFFFFF0) == 0x009FFFF0:
        print(f"  ROM offset 0x{i:06X}: ref to 0x{long_val:08X}")
        count += 1
        if count >= 10:
            break
