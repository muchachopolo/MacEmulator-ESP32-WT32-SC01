#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * V8 (LC II / Color Classic) floppy: high-level .Sony Prime replacement.
 *
 * The ROM's .Sony driver (LC II: DRVR header at $A6C3E0) keeps doing
 * Open/Control/Status/disk-insert detection itself, but its Prime entry is
 * just `MOVE.L JPrime($0226),-(SP); RTS`.  Once the driver's Open has filled
 * JPrime, v8disk_frame() points it at a 12-byte stub served from the
 * otherwise unmapped bus window V8DISK_BASE:
 *
 *   $FE0000  TST.B   $00FE0100      ; data read -> v8disk_prime() in C
 *   $FE0006  MOVE.L  JIODone($08FC),-(SP)
 *   $FE000A  RTS                    ; -> IODone with D0 = result
 *
 * v8disk_prime() copies sectors straight from the embedded image
 * (A0 = PB, A1 = DCE).  No per-instruction cost: the stub lives in the
 * slow path (mem_read_byte), reached only for unmapped addresses.
 */
#define V8DISK_BASE   0xFE0000u
#define V8DISK_END    0xFE1000u

/* mem_read_byte slow-path hook for V8DISK_BASE..V8DISK_END-1 */
uint8_t v8disk_read(uint32_t address);

/* Once per emulated frame: (re)install the JPrime redirect when needed */
void v8disk_frame(void);

/*
 * Second disk (MAC_DRIVE2, Raspberry Pi port): a platform-supplied volume
 * mounted as drive 2 of .Sony once the Finder is up. The LC II has a single
 * floppy, so the emulator adds the drive itself: at the Finder's next
 * WaitNextEvent/GetNextEvent (A-line pre-hook, like MAC_QUIT_REPAINT) it
 * runs a routine served from V8DISK_BASE+$40 that allocates a drive queue
 * element (_NewPtrSysClear), _AddDrive's it as drive 2 / refNum -5
 * (non-ejectable, size in dQDrvSz/dQDrvSz2) and posts diskEvt for drive 2;
 * the system then mounts it. Prime requests for drive 2 go to io().
 * 0 (default): no drive 2, exactly the previous behaviour.
 */
#ifndef MAC_DRIVE2
#define MAC_DRIVE2 0
#endif
/* io: transfer cnt bytes at image offset off; returns 0 or a Mac error */
typedef int (*v8disk_io_fn)(bool write, uint8_t *buf, uint32_t off, uint32_t cnt);
void v8disk_set_drive2(uint32_t size, v8disk_io_fn io, bool writable);

/* From the A-line pre-hook; 1 = handled (the install routine was entered) */
int v8disk_aline_pre(unsigned int ir);

#ifdef __cplusplus
}
#endif
