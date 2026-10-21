# Peripherals

Device models in `MacintoshESPEmulator/main/`. Addresses: [memory map](memory-map.md).
Switch defaults: [`CLAUDE.md`](../CLAUDE.md#build-switches).

| Device | Classic | LC II (V8) | File |
|---|---|---|---|
| VIA 6522 | yes | VIA1 | `mac_via.c` |
| ADB / clock / PRAM | VIA-SR bypass + RTC chip | Egret HLE (Cuda for Color Classic) | `mac_rtc.c`, `mac_egret.c` |
| Floppy | SWIM ISM + .Sony intercepts | SWIM (detection) + .Sony Prime hook | `mac_iwm.c`, `mac_memory.c`, `mac_v8disk.c` |
| Disk writes | PSRAM overlay | PSRAM overlay | `mac_disk_overlay.c` |
| Video | framebuffer in RAM | VDAC, pseudo-VIA2, VRAM | `mac_video.c`, `mac_v8video.c` |
| SCSI NCR 5380 | empty bus | empty bus | `mac_scsi.c` |
| SCC Z8530 | stub | stub | `mac_scc.c` |
| Sound (ASC / sound buffer) | not emulated | not emulated | — |

## VIA 6522 (`mac_via.c`)

Registers every $200 (Classic base $EFE1FE, V8 $F00000), index =
`(address >> 9) & 0xF`: ORB, ORA, DDRB, DDRA, T1CL/T1CH/T1LL/T1LH, T2CL/T2CH,
SR, ACR, PCR, IFR, IER, ORA (no handshake).

- **Interrupts:** CA1 = VBL (60.15 Hz), CA2 = one-second, T1, T2, SR. IFR bit 7
  = `(IFR & IER & $7F) != 0`, wired to 68k level 1. The CPU's IRQ line follows
  every IFR/IER change immediately (V8 always; Classic with
  `CLASSIC_VIA_IRQ_NOW`). Before that fix, the Classic re-took an
  already-cleared interrupt ~14 times per frame.
- **Timers:** T1 one-shot or free-running (ACR bit 6), T2 one-shot. They count
  at the **E clock (CPU/10)** with mid-slice interpolation (V8 always; Classic
  with `CLASSIC_VIA_ECLOCK` + `CLASSIC_VIA_INTERP`). The ROM calibrates delay
  loops (TimeDBRA etc.) against these timers; the old coarse model caused
  divide-by-zero crashes once boot timing shifted. On V8 device time is
  `MAC_CYCLE_SCALE` × CPU cycles. `via_tick()` runs after every slice, which
  is why `fast_slice` must stay ≤ 4000 cycles.
- **Classic port A:** bit 4 = overlay (the first ORA write clears it for good),
  bits 0–2 sound volume, bit 6 screen buffer select. On V8, port A carries the
  machine ID bits (Color Classic $0B, LC II $5D) and ORA does not touch the overlay.
- **Classic port B:** PB0–PB2 = RTC data/clock/enable, PB3 = mouse button
  (active low; IRB starts at $31, button down during boot, which the boot
  needs), PB4/PB5 forced high (ADB idle). On V8, PB3/PB4/PB5 are the Egret
  handshake lines.

## Classic ADB (bypass)

There is no ADB transceiver on the Classic build. The ADB polling loop is
satisfied by writing the completion flag at $08446C each frame, with PB4/PB5
high and emulated SR interrupts. Mouse input does not go through ADB: the
cursor globals are written directly and clicks are posted by a C mirror of the
ROM's ADB mouse handler ([display-pipeline](display-pipeline.md#touch-input)).
There is no keyboard yet.

## Classic RTC / PRAM (`mac_rtc.c`, `CLASSIC_RTC`)

The 343-0042-B clock chip, serial on VIA port B: 32-bit seconds counter
(ticked by the one-second interrupt) and 256 bytes of extended PRAM. With it
the Mac has a real clock, PRAM round-trips, and SPConfig ($22) no longer
triggers the AppleTalk alert. PRAM is not saved across power-off (planned:
NVS). `CLASSIC_RTC=0` restores the old behaviour (PB0 reads 1, PRAM $FF,
frozen clock).

## Egret / Cuda HLE (`mac_egret.c`, V8)

The LC II's Egret (Color Classic: Cuda, `MAC_V8_CUDA=1`) is a 6805
microcontroller behind VIA1: PB3 = TREQ (MCU → CPU, active low), PB4 =
BYTEACK, PB5 = TIP (active high on Egret, low on Cuda); bytes move through the
VIA shift register (ACR bit 4 = direction). The HLE implements the byte
handshake and the packet protocol, not the MCU's code:

- **PRAM** 256 bytes (including the LC II's 32-byte streaming reads) and the
  **RTC**. PRAM $01 = 1 at init cuts the SCSI startup-disk wait from 20 s to 1 s.
- **1 Hz tick packets** `[03][00][00]` after SET_ONE_SECOND_MODE
  (`EGRET_ONE_SEC_TICKS`): Egret while autopoll is on, Cuda when the mode is
  non-zero. They advance the Mac's Time global (it was frozen before).
- **ADB:** autopoll and an emulated ADB mouse that reports the button only (zero
  deltas); the position is written directly. An ADB keyboard (address 2) is planned.
- IIC and 6805 memory read/write commands answered as the ROM expects.

## Floppy and disk path

The disk image is embedded in flash (`DISK_NAME`, read-only) and always sits
in drive 1 as an internal floppy. Volumes up to the flash budget (2880K) mount
through the same path.

### Classic: SWIM + .Sony intercepts (`mac_iwm.c`, `mac_memory.c`)

- **IWM mode:** the state lines CA0–CA2, LSTRB, ENABLE, SELECT, Q6, Q7 select
  the sense line or register (drive present, disk in place, write-protect,
  track 0, motor, tach).
- **ISM mode** (SWIM's MFM interface): DATA, MARK, CRC, PARAM, PHASE, SETUP,
  STATUS/MODE0, HANDSHAKE/MODE1. The emulator synthesizes the MFM byte stream
  of a 1.44 MB track (gaps, $A1 sync marks, ID and data fields with
  CRC-CCITT) from the image, at ~125 CPU cycles per byte (500 kbps). A stream
  pointer advances with time and a read pointer with DATA reads; RDDATA is set
  while unread bytes exist. The ISM registers share the VIA window: A12 = 1
  with the PC in the SWIM driver ($434000–$436000) routes to the SWIM.
- **.Sony intercepts:** bulk `_Read` requests (ioRefNum −5) are served
  directly from the image at the driver's entry ($40315C), a spin-rescue at
  $4031BE completes async reads that missed it, and `_Write` is intercepted at
  $4031CC into the overlay. Async completion routines are called
  (`dm_async_complete`), which fixed the Shut Down freeze. Large images report
  their size through csCode 6 (format list).

### V8 floppy

`mac_v8disk.c` replaces only the .Sony Prime routine.

The ROM's .Sony driver does its own Open, Control, Status and disk-insert
detection; SWIM emulation (real IWM↔ISM switching, a 16-entry drive sense
table, drive 2 "not installed") is enough for that. Its Prime entry is
`MOVE.L JPrime($0226),-(SP); RTS`. After Open, `v8disk_frame()` points JPrime
at a 12-byte stub served from the unmapped window $FE0000:

```
$FE0000  TST.B  $00FE0100        ; this read runs v8disk_prime() in C
$FE0006  MOVE.L JIODone($08FC),-(SP)
$FE000A  RTS                     ; → IODone with D0 = result
```

`v8disk_prime()` copies sectors between the image/overlay and Mac RAM. Nothing
runs per instruction.

### Disk overlay

`mac_disk_overlay.c`: "level 1" writable disk: a block copy-on-write overlay in PSRAM
(`DISK_WRITABLE`, default 1). Each written 512-byte block gets a slot in a
pool of `DISK_OVERLAY_KB` (384 KB Classic, 256 KB LC II, whose VRAM uses
512 KB of PSRAM) plus a 2-byte-per-block index. Reads take the slot if there
is one, else the flash image. Writes identical to flash take no slot. A write
that would not fit fails as a whole with dskFulErr (−34) so the volume never
holds half a request. The volume mounts unlocked. **Everything is lost at
power-off or restart.** `DISK_WRITABLE=0` restores the read-only volume
(writes fail with wPrErr, volume locked). Level 2 (flash partition + flush +
PRAM in NVS) is planned.

## V8 video

`mac_v8video.c`:

- **VDAC / CLUT** at $F24000: +0 address, +1 data (R, G, B triplets), +2 control.
- **pseudo-VIA2** at $F26000: SlotIFR (active low, bit 6 = video VBL),
  IFR/IER, MonP (bits 0–2 depth, 3–5 monitor sense = 2 "12-inch RGB", bit 6
  video off). The VBL is pulsed each frame.
- **VRAM** 512 KB at $F40000 in PSRAM, ScrnBase $50F40000, rowBytes 1024, with
  8/16/32-bit fast paths in the memory callbacks.
- ROM VPBlock patch to 480×320 and the renderer: [display-pipeline](display-pipeline.md).

## SCSI NCR 5380 (`mac_scsi.c`)

Full register model with an empty bus: arbitration succeeds, selection always
times out. This keeps the SCSI Manager and apps like HD SC Setup from hanging.
On the Classic, all SCSI reads return $00 during early boot (`boot_mode`) and a
polling bypass at $41A44A stays active. A virtual SCSI disk is a possible
future item.

## SCC Z8530 (`mac_scc.c`)

A stub with the WR0 register pointer. RR0 = $7C (Tx empty, DCD/CTS inactive,
Sync/Hunt so the LocalTalk INIT does not loop), RR1 = $07 (all sent). Data
reads return $00, writes are ignored. No serial ports or networking.

## Shut Down and Restart

Classic Shut Down works (the "You may now switch off" screen); its Restart
button and Special → Restart cold-boot the ESP32 (`esp_restart()`).
