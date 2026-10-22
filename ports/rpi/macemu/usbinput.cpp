#include "usbinput.h"
#include <circle/usb/usbkeyboard.h>
#include <circle/input/mouse.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/util.h>
#include "mac_kbd.h"
#include "mac_touch.h"

static const char From[] = "usb";

// USB HID usage (keyboard page) -> ADB key code, 0xFF = unmapped.
static const u8 HIDToADB[0x68] =
{
	0xFF, 0xFF, 0xFF, 0xFF,				// 00-03
	0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04,	// a b c d e f g h
	0x22, 0x26, 0x28, 0x25, 0x2E, 0x2D, 0x1F, 0x23,	// i j k l m n o p
	0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07,	// q r s t u v w x
	0x10, 0x06,					// y z
	0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C,	// 1 2 3 4 5 6 7 8
	0x19, 0x1D,					// 9 0
	0x24, 0x35, 0x33, 0x30, 0x31,			// Return Esc Backspace Tab Space
	0x1B, 0x18, 0x21, 0x1E, 0x2A, 0x2A,		// - = [ ] \ (non-US #)
	0x29, 0x27, 0x32, 0x2B, 0x2F, 0x2C,		// ; ' ` , . /
	0x39,						// Caps Lock (handled as a lock)
	0x7A, 0x78, 0x63, 0x76, 0x60, 0x61,		// F1-F6
	0x62, 0x64, 0x65, 0x6D, 0x67, 0x6F,		// F7-F12
	0x69, 0x6B, 0x71,				// PrtScr ScrLk Pause = F13-F15
	0x72, 0x73, 0x74, 0x75, 0x77, 0x79,		// Insert=Help Home PgUp Del End PgDn
	0x3C, 0x3B, 0x3D, 0x3E,				// Right Left Down Up
	0x47, 0x4B, 0x43, 0x4E, 0x45, 0x4C,		// NumLock=Clear KP/ KP* KP- KP+ KPEnter
	0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5B,	// KP1-KP8
	0x5C, 0x52, 0x41,				// KP9 KP0 KP.
	0x0A, 0xFF, 0xFF, 0x51,				// ISO \ (64), App, Power, KP=
};

// USB modifier bits (LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI) -> ADB:
// Alt = Option, GUI (Windows key) = Command.
static const u8 ModToADB[8] =
{
	ADB_KEY_CONTROL, ADB_KEY_SHIFT, ADB_KEY_OPTION, ADB_KEY_COMMAND,
	ADB_KEY_CONTROL, ADB_KEY_SHIFT, ADB_KEY_OPTION, ADB_KEY_COMMAND,
};

static const u8 HIDCapsLock = 0x39;

static boolean Contains (const unsigned char Keys[6], u8 k)
{
	for (unsigned i = 0; i < 6; i++)
	{
		if (Keys[i] == k)
		{
			return TRUE;
		}
	}
	return FALSE;
}

volatile u32 CUSBInput::s_Queue[QueueSize];
volatile unsigned CUSBInput::s_nHead = 0;
volatile unsigned CUSBInput::s_nTail = 0;
CUSBInput *CUSBInput::s_pThis = 0;

CUSBInput::CUSBInput (CUSBHCIDevice *pUSBHCI)
:	m_pUSBHCI (pUSBHCI),
	m_pKeyboard (0),
	m_pMouse (0),
	m_nLastPnP (0)
{
	s_pThis = this;
}

void CUSBInput::Push (u32 nEvent)			// interrupt context
{
	unsigned nNext = (s_nHead + 1) % QueueSize;
	if (nNext != s_nTail)
	{
		s_Queue[s_nHead] = nEvent;
		s_nHead = nNext;
	}
}

void CUSBInput::KeyStatusHandler (unsigned char ucModifiers, const unsigned char RawKeys[6])
{
	static unsigned char s_ucMods;
	static unsigned char s_Keys[6];
	static boolean s_bCapsLocked;

	// Modifiers: an ADB modifier is down while either side's key is.
	for (unsigned i = 0; i < 4; i++)
	{
		boolean bWas = (s_ucMods & (0x11 << i)) != 0;
		boolean bNow = (ucModifiers & (0x11 << i)) != 0;
		if (bWas != bNow)
		{
			Push (ModToADB[i] | (bNow ? 0x100 : 0));
		}
	}
	s_ucMods = ucModifiers;

	// Releases, then presses (a key is identified by its usage code).
	for (unsigned i = 0; i < 6; i++)
	{
		u8 k = s_Keys[i];
		if (k < 4 || Contains (RawKeys, k))
		{
			continue;
		}
		if (k < sizeof HIDToADB && HIDToADB[k] != 0xFF && k != HIDCapsLock)
		{
			Push (HIDToADB[k]);
		}
	}
	for (unsigned i = 0; i < 6; i++)
	{
		u8 k = RawKeys[i];
		if (k < 4 || Contains (s_Keys, k))
		{
			continue;
		}
		if (k == HIDCapsLock)
		{
			// The Mac's Caps Lock key locks mechanically: down while engaged.
			s_bCapsLocked = !s_bCapsLocked;
			Push (ADB_KEY_CAPSLOCK | (s_bCapsLocked ? 0x100 : 0));
		}
		else if (k < sizeof HIDToADB && HIDToADB[k] != 0xFF)
		{
			Push (HIDToADB[k] | 0x100);
		}
	}
	memcpy (s_Keys, RawKeys, 6);
}

void CUSBInput::MouseStatusHandler (unsigned nButtons, int nDisplacementX, int nDisplacementY,
				    int nWheelMove)
{
	(void) nWheelMove;
	int dx = nDisplacementX < -2047 ? -2047 : nDisplacementX > 2047 ? 2047 : nDisplacementX;
	int dy = nDisplacementY < -2047 ? -2047 : nDisplacementY > 2047 ? 2047 : nDisplacementY;
	Push (0x80000000u | ((nButtons & MOUSE_BUTTON_LEFT) ? 0x1000000u : 0)
	      | ((u32) dx & 0xFFF) << 12 | ((u32) dy & 0xFFF));
}

void CUSBInput::KeyboardRemoved (CDevice *pDevice, void *pContext)
{
	s_pThis->m_pKeyboard = 0;
	CLogger::Get ()->Write (From, LogNotice, "keyboard removed");
}

void CUSBInput::MouseRemoved (CDevice *pDevice, void *pContext)
{
	s_pThis->m_pMouse = 0;
	CLogger::Get ()->Write (From, LogNotice, "mouse removed");
}

void CUSBInput::Poll (void)
{
	u64 nNow = CTimer::GetClockTicks64 ();
	if (nNow - m_nLastPnP >= 500000)
	{
		m_nLastPnP = nNow;
		if (m_pUSBHCI->UpdatePlugAndPlay ())
		{
			CDeviceNameService *pNames = CDeviceNameService::Get ();
			if (!m_pKeyboard)
			{
				CUSBKeyboardDevice *pKeyboard =
					(CUSBKeyboardDevice *) pNames->GetDevice ("ukbd1", FALSE);
				if (pKeyboard)
				{
					m_pKeyboard = pKeyboard;
					pKeyboard->RegisterRemovedHandler (KeyboardRemoved);
					pKeyboard->RegisterKeyStatusHandlerRaw (KeyStatusHandler);
					CLogger::Get ()->Write (From, LogNotice, "keyboard attached");
				}
			}
			if (!m_pMouse)
			{
				CMouseDevice *pMouse = (CMouseDevice *) pNames->GetDevice ("mouse1", FALSE);
				if (pMouse)
				{
					m_pMouse = pMouse;
					pMouse->RegisterRemovedHandler (MouseRemoved);
					pMouse->RegisterStatusHandler (MouseStatusHandler);
					CLogger::Get ()->Write (From, LogNotice, "mouse attached");
				}
			}
		}
	}

	// Deliver: key transitions in order; consecutive mouse reports merged.
	int nDX = 0, nDY = 0;
	boolean bMouse = FALSE, bButton = FALSE;
	while (s_nTail != s_nHead)
	{
		u32 nEvent = s_Queue[s_nTail];
		s_nTail = (s_nTail + 1) % QueueSize;
		if (nEvent & 0x80000000u)
		{
			boolean bNewButton = (nEvent & 0x1000000u) != 0;
			if (bMouse && bNewButton != bButton)
			{
				touch_mouse_input (nDX, nDY, bButton);	// keep the click
				nDX = nDY = 0;
			}
			nDX += (int) ((nEvent >> 12) & 0xFFF) << 20 >> 20;
			nDY += (int) (nEvent & 0xFFF) << 20 >> 20;
			bButton = bNewButton;
			bMouse = TRUE;
		}
		else
		{
			kbd_key ((u8) (nEvent & 0x7F), (nEvent & 0x100) != 0);
		}
	}
	if (bMouse)
	{
		touch_mouse_input (nDX, nDY, bButton);
	}
}
