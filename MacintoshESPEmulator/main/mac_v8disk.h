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

#ifdef __cplusplus
}
#endif
