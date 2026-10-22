// Platform layer for the emulator core on the Pi: the ESP-IDF shim services
// (shim/pi_platform.h) and the components/display API (display.h), backed by
// Circle, the ILI9486 and the XPT2046. Rendering and touch run on core 1
// (videocore.cpp); everything else here runs on core 0.
#include "kernel.h"
#include "videocore.h"
#include "pi_platform.h"
#include "display.h"
#include "mac_memory.h"
#include "mac_video.h"
#include <circle/timer.h>
#include <circle/startup.h>
#include <circle/util.h>
#include <stdarg.h>
#include <stdio.h>

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
	CKernel::Get ()->FlushDisk (TRUE);
	CKernel::Get ()->GetLogger ()->Write ("mac", LogNotice, "Mac restart -> reboot");
	CTimer::SimpleMsDelay (200);
	reboot ();
}

volatile unsigned pi_frame_count;			// read by core 1's freeze check

void pi_frame_hook (void)
{
	static u64 LastThrottle, LastLogSave;
	u64 Now = CTimer::GetClockTicks64 ();
	pi_frame_count++;
	if (Now - LastLogSave >= 3000000)
	{
		LastLogSave = Now;
		CKernel::Get ()->GetLogFile ()->Save ();
	}
	CKernel::Get ()->FlushDisk (FALSE);		// cheap unless the Mac wrote
	CKernel::Get ()->GetUSBInput ()->Poll ();	// USB keyboard + mouse
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
	CKernel::Get ()->GetLCD ()->Blit (x, y, w, h, data);	// core 1
	CVideoCore::s_nDraws++;
	CVideoCore::PollTouch ();				// keep ~60 Hz during big redraws
}

bool display_get_touch (int *x, int *y)			// core 0
{
	return CVideoCore::GetTouch (x, y);
}

void display_set_brightness (int percent)
{
	(void) percent;					// backlight is hard-wired on
}
