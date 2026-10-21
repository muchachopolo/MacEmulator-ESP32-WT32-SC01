# Macintosh 68000 Memory Map

The Macintosh Classic uses a 24-bit address bus (16MB address space). All peripherals are memory-mapped.

## Address Decode (mac_memory.c)

### Normal Mode (overlay = 0)

After the ROM startup code clears the overlay bit in the VIA, the memory map is:

```
0x000000 ┌──────────────────────┐
         │ RAM (2MB physical)   │  Mirrored in 4MB address window
         │ ram[address & mask]  │  Writable
0x400000 ├──────────────────────┤
         │ ROM (512KB)          │  Mirrored to fill 1MB
         │ rom[address & mask]  │  Read-only
0x500000 ├──────────────────────┤
         │ (unused)             │  Returns 0xFF
0x600000 ├──────────────────────┤
         │ (unused)             │
0x800000 ├──────────────────────┤
         │ SCC Read             │  Odd bytes only, even → 0xFF
         │ Zilog 8530           │  Bit 1: channel, Bit 2: data/ctrl
0xA00000 ├──────────────────────┤
         │ SCC Write            │  Odd bytes only
0xC00000 ├──────────────────────┤
         │ IWM/SWIM             │  Odd bytes only
         │ Floppy controller    │  A1-A4 select register/line
0xE00000 ├──────────────────────┤
         │ VIA 6522             │  Odd bytes only
         │                      │  Register N at base + N*0x200
0xF00000 ├──────────────────────┤
         │ (unused)             │
0xFFFFFF └──────────────────────┘
```

### Overlay Mode (overlay = 1, after reset)

On reset, the VIA overlay bit is set. This remaps ROM to address 0 so the 68000 can read its reset vector (which must be at 0x000000):

```
0x000000 ┌──────────────────────┐
         │ ROM (mapped here)    │  Reset vectors at 0x000000
0x400000 ├──────────────────────┤
         │ ROM (normal loc)     │
0x500000 ├──────────────────────┤
         │ (unused)             │
0x600000 ├──────────────────────┤
         │ RAM (alternate)      │  RAM accessible here during overlay
0x700000 ├──────────────────────┤
         │ ...peripherals same  │
```

The ROM startup code writes to VIA Port A bit 4 to clear overlay, which switches RAM back to address 0.

## Special RAM Addresses

The Mac stores special data structures at fixed offsets relative to the top of RAM. With 2MB physical RAM (`MAC_RAM_SIZE = 0x200000`):

| Address (2MB) | Offset from top | Content |
|---------------|-----------------|---------|
| 0x1FA700 | RAM_SIZE - 0x5900 | Screen buffer start (21,888 bytes) |
| 0x1FFD00 | RAM_SIZE - 0x0300 | Sound buffer (768 bytes) |

The screen buffer is 512 * 342 / 8 = 21,888 bytes of 1-bit pixel data. Each scanline is 64 bytes (512 pixels / 8). Bit 7 is the leftmost pixel. 1 = black, 0 = white.

## Implementation Notes

- All address decoding masks to 24 bits: `address &= 0x00FFFFFF`
- RAM and ROM use bitmask mirroring: `address & (size - 1)` (requires power-of-2 sizes)
- Peripheral registers respond only on odd byte addresses (the 68000 data bus routing)
- Word and long reads are constructed from consecutive byte reads (big-endian)
- Writes to ROM space are silently ignored
- Unmapped addresses return 0xFF (open bus)

## V8 Family Map (Color Classic / LC II, `MAC_ROM_LC_II=1`)

From the ROM's own decoder table (Color-Classic.ROM file $3490; LC-II.ROM holds the same
constants). The 32-bit bases are $50Fxxxxx; the 24-bit bus drops the top byte.

| Address | Device | Emulated |
|---------|--------|----------|
| $000000-$9FFFFF | RAM (2MB physical, aliased via mask) | yes |
| $A00000-$AFFFFF | ROM ($40A00000) | yes |
| $F00000-$F01FFF | VIA1 (registers every $200) | yes |
| $F04000 | SCC | no |
| $F06000 / $F10000 / $F12000 | SCSI DRQ / SCSI / SCSI handshake | no |
| $F14000 | ASC (sound) | no |
| $F16000-$F17FFF | SWIM | yes |
| $F24000 | VDAC (CLUT) | TODO |
| $F26000 | pseudo-VIA2 / RBV (video enable, monitor sense) | TODO |
| $F40000-$FBFFFF | VRAM | TODO |
| $FC0000 | diagnostic-ROM probe (nothing there) | reads $FF |

Overlay: after reset the ROM is mirrored at $0-$7FFFF. It switches off on the first access
to $A00000-$AFFFFF (the ROM jumps there at $A02E3E). VIA ORA writes do NOT clear it on V8.
The Egret microcontroller (ADB, RTC, PRAM) talks through the VIA1 shift register with
PB3/PB4/PB5 handshake lines.
