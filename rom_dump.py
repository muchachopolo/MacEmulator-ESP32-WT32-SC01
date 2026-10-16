import struct

with open('D:/Personal/Macintosh/ROM/Classic.ROM', 'rb') as f:
    rom = f.read()

def dump_area(offset, length, label):
    print(f"\n{label}:")
    for i in range(0, length, 2):
        addr = 0x400000 + offset + i
        word = (rom[offset+i] << 8) | rom[offset+i+1]
        desc = ""
        if word == 0x4E71: desc = "NOP"
        elif word == 0x4E75: desc = "RTS"
        elif word == 0x4E73: desc = "RTE"
        elif word == 0x60FE: desc = "BRA.S * (infinite loop!)"
        elif (word & 0xFF00) == 0x6000 and (word & 0xFF) != 0:
            disp = word & 0xFF
            if disp > 0x7F: disp -= 0x100
            desc = f"BRA.S *+{disp+2}"
        elif (word & 0xFF00) == 0x6600 and (word & 0xFF) != 0:
            disp = word & 0xFF
            if disp > 0x7F: disp -= 0x100
            desc = f"BNE.S *+{disp+2}"
        elif (word & 0xFF00) == 0x6700 and (word & 0xFF) != 0:
            disp = word & 0xFF
            if disp > 0x7F: disp -= 0x100
            desc = f"BEQ.S *+{disp+2}"
        elif (word & 0xFF00) == 0x6A00 and (word & 0xFF) != 0:
            disp = word & 0xFF
            if disp > 0x7F: disp -= 0x100
            desc = f"BPL.S *+{disp+2}"
        elif (word & 0xFF00) == 0x6B00 and (word & 0xFF) != 0:
            disp = word & 0xFF
            if disp > 0x7F: disp -= 0x100
            desc = f"BMI.S *+{disp+2}"
        print(f"  {addr:06X}: {word:04X}  {desc}")

# PC=0x4026DA -> ROM offset 0x26DA
dump_area(0x26C0, 48, "Around stuck PC=0x4026DA")

# PC=0x40222E -> ROM offset 0x222E
dump_area(0x2220, 32, "Around PC=0x40222E")

# Entry point area
dump_area(0x0020, 64, "ROM entry at 0x40002A")

# Check for the classic Mac memory test / overlay clearing code
# Search for VIA access pattern (writes to 0xEFxxxx)
print("\nSearching for VIA overlay clear pattern...")
for i in range(0, len(rom)-4, 2):
    # Look for BCLR or BSET instructions near VIA addresses
    word = (rom[i] << 8) | rom[i+1]
    # Look for move to VIA ORA pattern
    if i < len(rom)-8:
        # Check for references to 0xEFE1FE (VIA base)
        long_val = struct.unpack_from('>I', rom, i)[0]
        if long_val == 0x00EFE1FE or long_val == 0xEFE1FE00:
            print(f"  Found VIA base ref at ROM offset 0x{i:06X}")
        # Check for 0xEFFFFE (VIA ORA_NH = reg 15)
        if long_val == 0x00EFFFFE or long_val == 0xEFFFFE00:
            print(f"  Found VIA ORA_NH ref at ROM offset 0x{i:06X}")
