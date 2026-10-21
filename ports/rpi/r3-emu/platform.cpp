// Platform layer for the emulator core on the Pi: the ESP-IDF shim services
// (shim/pi_platform.h) and the components/display API (display.h), backed by
// Circle, the ILI9486 and the XPT2046.
#include "kernel.h"
#include "pi_platform.h"
#include "display.h"
#include "mac_memory.h"
#include "mac_video.h"
#include <circle/timer.h>
#include <circle/startup.h>
#include <circle/util.h>
#include <stdarg.h>
#include <stdio.h>

// Single core: the LCD is refreshed from the frame hook at most this often.
// A changed 20-line strip costs ~8.6 ms at 17.9 MHz and blocks the 68k.
static const u64 RenderIntervalUs = 33000;
static const u64 ThrottleIntervalUs = 3000000;	// CCPUThrottle temperature check

uint64_t pi_micros (void)
{
	return CTimer::GetClockTicks64 ();
}

void pi_delay_ms (unsigned ms)
{
	CTimer::SimpleMsDelay (ms);
}

void pi_log (int level, const char *tag, const char *fmt, ...)
{
	char Buf[256];
	va_list ap;
	va_start (ap, fmt);
	vsnprintf (Buf, sizeof Buf, fmt, ap);
	va_end (ap);
	TLogSeverity Severity = level <= 1 ? LogError : level == 2 ? LogWarning : LogNotice;
	CKernel::Get ()->GetLogger ()->Write (tag, Severity, "%s", Buf);
}

void pi_restart (void)
{
	CKernel::Get ()->GetLogger ()->Write ("r3", LogNotice, "Mac restart -> reboot");
	CTimer::SimpleMsDelay (200);
	reboot ();
}

void pi_frame_hook (void)
{
	static u64 LastRender, LastThrottle;
	u64 Now = CTimer::GetClockTicks64 ();
	if (Now - LastRender >= RenderIntervalUs)
	{
		LastRender = Now;
		uint8_t *pRAM = mac_mem_get_ram ();
		if (pRAM)
		{
			mac_video_render (pRAM, mac_mem_get_ram_size ());
		}
	}
	if (Now - LastThrottle >= ThrottleIntervalUs)
	{
		LastThrottle = Now;
		CKernel::Get ()->GetCPUThrottle ()->Update ();
	}
}

// --- components/display API -------------------------------------------------

bool display_init (void)
{
	CILI9486 *pLCD = CKernel::Get ()->GetLCD ();
	pLCD->SetClock (CILI9486::MaxClock);
	pLCD->Initialize ();
	pLCD->Fill (0, 0, CILI9486::Width, CILI9486::Height, 0);
	return true;
}

void display_draw (int x, int y, int w, int h, const uint16_t *data)
{
	CKernel::Get ()->GetLCD ()->Blit (x, y, w, h, data);
}

// The emulator expects the WT32-SC01's FT5x06 orientation: tx grows with
// screen y, ty shrinks as screen x grows (mac_touch.c: dx = -dty, dy = dtx).
bool display_get_touch (int *x, int *y)
{
	unsigned rx, ry, rz;
	if (!CKernel::Get ()->GetTouch ()->Read (&rx, &ry, &rz))
	{
		return false;
	}
	int u = XPT2046_CAL_SWAP ? ry : rx, v = XPT2046_CAL_SWAP ? rx : ry;
	int sx = 40 + (u - XPT2046_CAL_LEFT) * (int) (CILI9486::Width - 80)
			/ (XPT2046_CAL_RIGHT - XPT2046_CAL_LEFT);
	int sy = 40 + (v - XPT2046_CAL_TOP) * (int) (CILI9486::Height - 80)
			/ (XPT2046_CAL_BOTTOM - XPT2046_CAL_TOP);
	*x = sy;
	*y = (int) CILI9486::Width - 1 - sx;
	return true;
}

void display_set_brightness (int percent)
{
	(void) percent;					// backlight is hard-wired on
}
