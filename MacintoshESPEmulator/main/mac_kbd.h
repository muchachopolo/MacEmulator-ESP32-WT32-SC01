#pragma once

/*
 * Keyboard input (MAC_KEYBOARD). The platform reports keys as ADB keyboard
 * key codes (Apple Extended Keyboard raw codes: 'a' $00, Return $24,
 * Control $36, Command $37, Shift $38, Caps Lock $39, Option $3A, arrows
 * $3B-$3E ...); see mac_kbd.c for the table.
 *
 *  - LC II (V8): an ADB keyboard at address 2 on the emulated Egret/Cuda
 *    (mac_egret.c). The Mac's own ADB keyboard driver translates the codes
 *    (KMAP, KCHR), keeps KeyMap and generates auto-key events.
 *  - Classic: the emulator has no ADB bus, so kbd_frame() mirrors the ROM's
 *    ADB keyboard handler ($403AC0): KeyMap, KeyLast, KeyTime/KeyRepTime,
 *    the auto-key message ($0216) and _PostEvent keyDown/keyUp, with a
 *    built-in US layout instead of KeyTrans. The OS's GetOSEvent then makes
 *    the auto-key events as usual.
 *
 * MAC_KEYBOARD 0 (default on the ESP32, which has no keyboard): no keyboard
 * device on the LC II's ADB, nothing posted on the Classic -- exactly the
 * previous behaviour.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MAC_KEYBOARD
#define MAC_KEYBOARD 0
#endif

#define ADB_KEY_CONTROL  0x36
#define ADB_KEY_COMMAND  0x37
#define ADB_KEY_SHIFT    0x38
#define ADB_KEY_CAPSLOCK 0x39
#define ADB_KEY_OPTION   0x3A

/* Queue a key transition (emulation-loop context: not from an interrupt). */
void kbd_key(uint8_t adb_code, bool down);

/* Emulation loop, once per frame (Classic: posts queued keys). */
void kbd_frame(void);

/* LC II Egret ADB keyboard (address 2, handler 1). */
bool kbd_adb_pending(void);
void kbd_adb_take_r0(uint8_t out[2]);   /* up to two key bytes, $FF = none */
void kbd_adb_untake(void);              /* the packet was abandoned */
void kbd_adb_reset(void);
uint8_t kbd_adb_addr(void);
void kbd_adb_listen_r3(uint8_t a0, uint8_t a1);
uint8_t kbd_adb_handler(void);

#ifdef __cplusplus
}
#endif
