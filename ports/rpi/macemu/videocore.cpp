#include "videocore.h"
#include "kernel.h"
#include "mac_memory.h"
#include "mac_video.h"
#include <circle/timer.h>
#include <circle/string.h>
#include "m68k.h"

extern volatile unsigned pi_frame_count;		// platform.cpp

// Freeze check: if the 68k has not finished a frame for this long, show the
// frame count and its PC on the bottom of the LCD (for photos/bug reports).
static const u64 FreezeUs = 5000000;

// Touch sampling period: the ESP32's sampler runs at ~60 Hz.
static const u64 TouchPeriodUs = 16000;

volatile unsigned CVideoCore::s_nDraws = 0;
volatile u32 CVideoCore::s_nTouch = 0;

CVideoCore::CVideoCore (CMemorySystem *pMemorySystem)
:	CMultiCoreSupport (pMemorySystem)
{
}

void CVideoCore::Run (unsigned nCore)
{
	if (nCore != 1)
	{
		return;						// cores 2 and 3 are idle
	}
	unsigned nLastFrame = pi_frame_count;
	u64 nLastProgress = CTimer::GetClockTicks64 (), nLastShown = 0;
	for (;;)
	{
		u64 nNow = CTimer::GetClockTicks64 ();
		if (pi_frame_count != nLastFrame)
		{
			nLastFrame = pi_frame_count;
			nLastProgress = nNow;
		}
		else if (nNow - nLastProgress >= FreezeUs && nNow - nLastShown >= 2000000)
		{
			nLastShown = nNow;
			CString Text;
			Text.Format ("%u-%06X", nLastFrame, m68k_get_reg (0, M68K_REG_PC) & 0xFFFFFF);
			CILI9486 *pLCD = CKernel::Get ()->GetLCD ();
			pLCD->Fill (0, CILI9486::Height - 28, CILI9486::Width, 28, 0xF800);
			pLCD->DrawDigits (6, CILI9486::Height - 24, 4, Text, 0xFFFF);
		}
		PollTouch ();
		unsigned nBefore = s_nDraws;
		uint8_t *pRAM = mac_mem_get_ram ();
		if (pRAM)
		{
			mac_video_render (pRAM, mac_mem_get_ram_size ());
		}
		if (s_nDraws == nBefore)
		{
			CTimer::SimpleMsDelay (2);		// nothing changed
		}
	}
}

void CVideoCore::PollTouch (void)
{
	static u64 s_nLast;
	u64 nNow = CTimer::GetClockTicks64 ();
	if (nNow - s_nLast < TouchPeriodUs)
	{
		return;
	}
	s_nLast = nNow;

	unsigned rx, ry, rz;
	if (!CKernel::Get ()->GetTouch ()->Read (&rx, &ry, &rz))
	{
		s_nTouch = 0;
		return;
	}
	const int W = CILI9486::Width, H = CILI9486::Height;
	int u = XPT2046_CAL_SWAP ? ry : rx, v = XPT2046_CAL_SWAP ? rx : ry;
	int sx = 40 + (u - XPT2046_CAL_LEFT) * (W - 80) / (XPT2046_CAL_RIGHT - XPT2046_CAL_LEFT);
	int sy = 40 + (v - XPT2046_CAL_TOP) * (H - 80) / (XPT2046_CAL_BOTTOM - XPT2046_CAL_TOP);
	sx = sx < 0 ? 0 : sx >= W ? W - 1 : sx;
	sy = sy < 0 ? 0 : sy >= H ? H - 1 : sy;
	// FT5x06 orientation: tx grows with screen y, ty shrinks as screen x grows
	// (mac_touch.c: dx = -dty, dy = dtx).
	s_nTouch = 0x80000000u | (u32) sy << 16 | (u32) (W - 1 - sx);
}

boolean CVideoCore::GetTouch (int *pX, int *pY)
{
	u32 nTouch = s_nTouch;					// one atomic read
	if (!(nTouch & 0x80000000u))
	{
		return FALSE;
	}
	*pX = (nTouch >> 16) & 0x7FFF;
	*pY = nTouch & 0xFFFF;
	return TRUE;
}
