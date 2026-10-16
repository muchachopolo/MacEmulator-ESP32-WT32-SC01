# Peripheral Emulation

## VIA 6522 (mac_via.c)

The Versatile Interface Adapter is the central I/O chip of the Macintosh. It handles interrupts, timers, keyboard, mouse, and system control.

### Register Map

VIA registers are at base address `0xEFE1FE`, spaced 512 bytes apart. Only odd byte addresses are active.

Register address = `0xEFE1FE + (register_number * 0x200)`

The address decode in `mac_memory.c` extracts the register number:
```c
int reg = (address >> 9) & 0xF;
```

| Reg | Name | Description |
|-----|------|-------------|
| 0 | ORB/IRB | Port B output/input (keyboard clock/data) |
| 1 | ORA/IRA | Port A output/input (overlay, volume, screen select) |
| 2 | DDRB | Port B data direction |
| 3 | DDRA | Port A data direction |
| 4 | T1CL | Timer 1 counter low (read clears T1 IRQ) |
| 5 | T1CH | Timer 1 counter high (write starts timer) |
| 6 | T1LL | Timer 1 latch low |
| 7 | T1LH | Timer 1 latch high |
| 8 | T2CL | Timer 2 counter low (read clears T2 IRQ) |
| 9 | T2CH | Timer 2 counter high (write starts timer) |
| 10 | SR | Shift register (keyboard data) |
| 11 | ACR | Auxiliary control (timer modes) |
| 12 | PCR | Peripheral control |
| 13 | IFR | Interrupt flag register |
| 14 | IER | Interrupt enable register |
| 15 | ORA_NH | Port A (no handshake) |

### Port A Bits

| Bit | Direction | Function |
|-----|-----------|----------|
| 0-2 | Output | Sound volume (0-7) |
| 3 | Output | (unused in Classic) |
| 4 | Output | **Overlay control** (1=ROM at 0x000000) |
| 5 | Output | Disk SEL line |
| 6 | Output | Screen buffer select (alternate/main) |
| 7 | Input | SCC wait/request |

### Interrupts

The VIA generates an IRQ (active low) when `(IFR & IER) != 0`. The Mac connects this to the 68000 IRQ level 1.

| IRQ Source | IFR Bit | Trigger |
|------------|---------|---------|
| VBlank | CA1 (bit 1) | Set every ~16.6ms by `via_vblank()` |
| One-second | CA2 (bit 0) | Not yet implemented |
| Timer 1 | T1 (bit 6) | Counter underflow |
| Timer 2 | T2 (bit 5) | Counter underflow |
| Shift register | SR (bit 2) | Keyboard byte complete |

### Timer Implementation

Timers are 16-bit (`uint16_t`) countdown counters. They are decremented by `via_tick(cycles)` each CPU time slice. Underflow is detected by checking if the counter wrapped around (new value > old value after subtraction).

Timer 1 supports free-running mode (ACR bit 6): on underflow, reloads from latch and continues counting.

## SCC — Zilog 8530 (mac_scc.c) — STUB

The Serial Communications Controller handles the modem and printer serial ports. On the Mac, it also relays mouse movement via the quadrature signals.

**Current implementation**: Returns safe defaults to prevent bus errors.
- RR0: Tx buffer empty (0x04)
- RR1: All sent (0x01)
- Data reads: 0x00
- All writes: silently ignored

### Address Decode

- Read: `0x800000 - 0x9FFFFF` (odd bytes)
- Write: `0xA00000 - 0xBFFFFF` (odd bytes)
- Bit 1: channel select (0=B, 1=A)
- Bit 2: register type (0=control, 1=data)

### Future Work

To support mouse movement properly, the SCC needs to:
1. Report mouse delta via the quadrature interface
2. Generate SCC interrupts for mouse movement
3. Currently mouse position is set directly (works for basic testing but not accurate)

## IWM/SWIM — Floppy Controller (mac_iwm.c)

The Integrated Woz Machine controls the 3.5" floppy drive. The Mac Classic uses the SWIM chip (backwards-compatible).

### Address Decode

IWM is at `0xC00000 - 0xDFFFFF` (odd bytes). Address lines A1-A4 select register/line:

```
Register = (address >> 1) & 0xF
Even register numbers: clear the line
Odd register numbers: set the line
```

### State Lines

| Line | Bit | Purpose |
|------|-----|---------|
| CA0 | 0x01 | Control line A0 |
| CA1 | 0x02 | Control line A1 |
| CA2 | 0x04 | Control line A2 |
| LSTRB | 0x08 | Latch strobe |
| ENABLE | 0x10 | Motor enable |
| SELECT | 0x20 | Drive select |
| Q6 | 0x40 | State machine control |
| Q7 | 0x80 | State machine control |

### Read Modes (based on Q6/Q7)

| Q6 | Q7 | Mode |
|----|-----|------|
| 0 | 0 | Read data / sense lines (based on CA0-CA2) |
| 1 | 0 | Read status register |
| 0 | 1 | Read handshake register |
| 1 | 1 | (write mode register) |

### Sense Lines (Q6=0, Q7=0)

When reading data, CA0-CA2 select which sense line to return:

| CA[2:0] | Sense | Value |
|---------|-------|-------|
| 0 | /STEP direction | 0x00 |
| 1 | Disk in place | 0x00 = inserted |
| 2 | /STEP (not at track 0) | 0x00 = not at track 0 |
| 3 | /WRPROT | 0xFF = write protected |
| 4 | Motor running | 0x00 |
| 6 | TACH | 0x00 |

### Current Limitations

The IWM currently only returns sense line status — it does not implement sector-level disk reads. The ROM will detect the disk as present and write-protected, but cannot read the boot blocks. Full floppy emulation requires:

1. GCR/MFM encoding emulation
2. Track/sector data serving from the embedded .dsk image
3. Proper timing for the Macintosh disk rotation speed
