// USB keyboard and mouse (Circle's DWHCI host stack) for the emulated Mac.
// The USB handlers run in interrupt context on core 0 and only fill a ring;
// Poll() runs from the emulator's frame hook and hands the events to the
// core: keys as ADB key codes (kbd_key, mac_kbd.c), the mouse as relative
// movement + button (touch_mouse_input, mac_touch.c).
#ifndef _usbinput_h
#define _usbinput_h

#include <circle/usb/usbhcidevice.h>
#include <circle/devicenameservice.h>
#include <circle/types.h>

class CUSBInput
{
public:
	CUSBInput (CUSBHCIDevice *pUSBHCI);

	// Frame hook (core 0, not in an interrupt): plug-and-play every 0.5 s,
	// then deliver the queued events.
	void Poll (void);

private:
	static void KeyStatusHandler (unsigned char ucModifiers, const unsigned char RawKeys[6]);
	static void MouseStatusHandler (unsigned nButtons, int nDisplacementX, int nDisplacementY,
					int nWheelMove);
	static void KeyboardRemoved (CDevice *pDevice, void *pContext);
	static void MouseRemoved (CDevice *pDevice, void *pContext);

	static void Push (u32 nEvent);

	CUSBHCIDevice *m_pUSBHCI;
	CDevice *m_pKeyboard;
	CDevice *m_pMouse;
	u64 m_nLastPnP;

	// Events: bit 31 = mouse (dx/dy 12 bits each, button bit 24),
	// else ADB key code | 0x100 if down.
	static const unsigned QueueSize = 256;
	static volatile u32 s_Queue[QueueSize];
	static volatile unsigned s_nHead, s_nTail;

	static CUSBInput *s_pThis;
};

#endif
