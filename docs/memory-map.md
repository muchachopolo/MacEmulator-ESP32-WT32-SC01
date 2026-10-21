# Memory maps

Both machines use a 24-bit bus (`address &= 0x00FFFFFF`; the LC II runs the
ROM in 24-bit mode, and its 32-bit device bases $50Fxxxxx reduce to
$Fxxxxx). Decoding is in `main/mac_memory.c` (`mem_read_byte` /
`mem_write_byte`, plus fast paths for RAM, ROM and VRAM). Unmapped addresses
read $FF; writes to ROM are ignored. Word and long accesses are big-endian.
Peripheral details: [peripherals](peripherals.md).

## Mac Classic (`MAC_ROM_LC_II=0`)

### Normal mode (overlay off)

| Range | Device | Notes |
|---|---|---|
| $000000–$3FFFFF | RAM (2 MB) | Aliased via `ram_mask` = $1FFFFF, like real hardware, so the ROM sizes 2 MB |
| $400000–$4FFFFF | ROM (512 KB) | Aliased via `rom_mask`; patched on read (see below) |
| $580000–$5FFFFF | SCSI NCR 5380 | Register = A6–A4; empty bus |
| $800000–$9FFFFF | SCC read | A1 = channel, A2 = data/control |
| $A00000–$BFFFFF | SCC write | Reads return $FF |
| $C00000–$DFFFFF | IWM/SWIM | Odd bytes; A1–A4 select the line/register |
| $E00000–$EFFFFF | VIA (registers at $EFE1FE + n×$200) | A12 = 1 **and** PC in the SWIM driver ($434000–$436000) routes to the SWIM ISM instead |

### Overlay mode (after reset)

ROM is mirrored at $000000 so the 68000 can fetch its reset vectors; RAM is
reachable at $600000–$7FFFFF. The first VIA ORA write clears the overlay for
good (one-shot latch). If the overlay is turned on again later, vectors
$000–$3FF still come from RAM.

### ROM patches

Applied in the ROM read paths (the flash image is never modified): checksum
and RAM-test skips (Mini vMac approach), the StartUpScreen BNE→BRA, and the
native 480×320 immediates ([display-pipeline](display-pipeline.md)).

### RAM layout (2 MB)

| Address | Contents |
|---|---|
| $000–$3FF | Exception vectors |
| $100–$FFF | Low-memory globals (overlapping the trap tables below) |
| $400–$7FF | OS trap table |
| $E00–$15FF | Toolbox trap table (System 7 extends it through the table at ($0664)) |
| $1600+ | System heap, then application heap |
| MemTop − $5900 = $1FA700 | Screen buffer, 512×342×1 bit (21,888 bytes, rowBytes 64; native mode shows 480×320 of it) |
| MemTop − $300 = $1FFD00 | Sound buffer |

## V8: LC II / Color Classic (`MAC_ROM_LC_II=1`)

From the ROM's own decoder table (Color-Classic.ROM file $3490; LC-II.ROM
uses the same constants).

| Range | Device | Emulated |
|---|---|---|
| $000000–$9FFFFF | RAM (2 MB populated; above `ram_size` reads $FF, no aliasing) | yes |
| $A00000–$AFFFFF | ROM ($40A00000) | yes; the declaration-ROM area is served from the patched 480×320 copy |
| $F00000–$F01FFF | VIA1 (registers every $200) | yes |
| $F04000–$F05FFF | SCC | stub |
| $F10000–$F13FFF | SCSI NCR 5380 (+ $F12000 pseudo-DMA) | empty bus |
| $F14000 | ASC (sound) | no (reads $FF) |
| $F16000–$F17FFF | SWIM | yes |
| $F24000 | VDAC (CLUT) | yes |
| $F26000 | pseudo-VIA2 (slot VBL, monitor sense, depth) | yes |
| $F40000–$FBFFFF | VRAM, 512 KB (in PSRAM) | yes, with 8/16/32-bit fast paths |
| $FC0000 | Diagnostic-ROM probe | reads $FF |
| $FE0000–$FE0FFF | Emulator-only: .Sony Prime stub ([peripherals](peripherals.md#v8-floppy)) | yes |

**Overlay:** after reset the ROM is mirrored at $000000–$07FFFF only (RAM
never moves to $600000). The overlay clears on the first access to
$A00000–$AFFFFF (the LC II ROM jumps there at $A02E3E), as in MAME; VIA ORA
writes do not clear it on V8.

**Machine identity:** VIA1 port A ID bits: Color Classic $0B, LC II $5D.

## Key low-memory globals

The emulator reads and writes these directly. A Point is 4 bytes, vertical
first: Y.hi Y.lo X.hi X.lo.

| Address | Name | Use in the emulator |
|---|---|---|
| $0108 | MemTop | Screen base on the Classic |
| $014A | EvQHdr | Event queue (click posting); qHead $014C, qTail $0150 |
| $016A | Ticks | Tick-wait detection (idle fast-forward), event times |
| $0172 | MBState | Button: $00 down, $80 up (whole byte) |
| $020C | Time | Mac clock (host prints it every 3000 frames) |
| $0226 | JPrime | V8 .Sony Prime hook |
| $0824 | ScrnBase | Classic framebuffer; $50F40000 on V8 |
| $0828 / $082C / $0830 | MTemp / RawMouse / Mouse | Cursor position (touch) |
| $0834 | CrsrPin | Cursor bounds (480×320 native; widened in game full view) |
| $08CE / $08CF | CrsrNew / CrsrCouple | Cursor redraw / coupling |
| $08FC | JIODone | I/O completion (V8 disk stub) |
| $0910 | CurApName | Current app (game mode, Finder detection) |
| $09D6 | WindowList | Game mode (no windows) |
| $09DE | WMgrPort | Window Manager ready gate for cursor writes |
