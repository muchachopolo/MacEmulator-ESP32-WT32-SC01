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
	if (level > 1 && !CKernel::Get ()->IsLogEnabled ())
	{
		return;					// log=0: errors only (kept for a panic)
	}
	va_list ap;
	va_start (ap, fmt);
	vsnprintf (Buf, sizeof Buf, fmt, ap);
	va_end (ap);
	TLogSeverity Severity = level <= 1 ? LogError : level == 2 ? LogWarning : LogNotice;
	CKernel::Get ()->GetLogger ()->Write (tag, Severity, "%s", Buf);
}

void pi_restart (void)
{
	CKernel::Get ()->GetLogger ()->Write ("mac", LogNotice, "Mac restart -> reboot");
	CKernel::Get ()->FlushDisk (TRUE);		// overlay -> image
	CKernel::Get ()->GetSDWorker ()->SyncAll ();	// image -> card, log
	reboot ();
}

volatile unsigned pi_frame_count;			// read by core 1's freeze check

void pi_frame_hook (void)
{
	static u64 LastThrottle;
	u64 Now = CTimer::GetClockTicks64 ();
	pi_frame_count++;
	CKernel::Get ()->FlushDisk (FALSE);		// RAM only; core 2 writes the card
	CKernel::Get ()->GetUSBInput ()->Poll ();	// USB keyboard + mouse
	CKernel::Get ()->PumpAudio ();			// ASC samples -> HDMI
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

// --- HDMI mirror: the 480x320 Mac screen, integer-scaled and centred, into
// Circle's 16-bit (RGB565) frame buffer.
static uint8_t *s_pHdmi;
static unsigned s_nHdmiPitch, s_nHdmiScale, s_nHdmiX, s_nHdmiY;

void hdmi_start (void)
{
	CBcmFrameBuffer *pFB = CKernel::Get ()->GetScreen ()->GetFrameBuffer ();
	if (!pFB || pFB->GetDepth () != 16 || !pFB->GetBuffer ())
	{
		return;
	}
	unsigned W = pFB->GetWidth (), H = pFB->GetHeight ();
	unsigned nScale = W / CILI9486::Width < H / CILI9486::Height ? W / CILI9486::Width
								       : H / CILI9486::Height;
	s_nHdmiScale = nScale ? nScale : 1;
	s_nHdmiPitch = pFB->GetPitch ();
	s_nHdmiX = W > CILI9486::Width * s_nHdmiScale ? (W - CILI9486::Width * s_nHdmiScale) / 2 : 0;
	s_nHdmiY = H > CILI9486::Height * s_nHdmiScale ? (H - CILI9486::Height * s_nHdmiScale) / 2 : 0;
	memset ((void *) (uintptr) pFB->GetBuffer (), 0, s_nHdmiPitch * H);
	s_pHdmi = (uint8_t *) (uintptr) pFB->GetBuffer ();
	CKernel::Get ()->GetLogger ()->Write ("mac", LogNotice, "HDMI %ux%u, Mac screen x%u", W, H, s_nHdmiScale);
}

static void hdmi_draw (int x, int y, int w, int h, const uint16_t *data)
{
	const unsigned s = s_nHdmiScale;
	const unsigned nMaxX = s_nHdmiPitch / 2;
	for (int row = 0; row < h; row++)
	{
		uint16_t *pDst = (uint16_t *) (s_pHdmi + (s_nHdmiY + (y + row) * s) * s_nHdmiPitch)
				 + s_nHdmiX + x * s;
		const uint16_t *pSrc = data + row * w;
		if (s_nHdmiX + (x + w) * s > nMaxX)
		{
			return;					// frame buffer narrower than 480
		}
		for (int i = 0; i < w; i++)
		{
			for (unsigned k = 0; k < s; k++)
			{
				*pDst++ = pSrc[i];
			}
		}
		pDst -= w * s;
		for (unsigned r = 1; r < s; r++)		// replicate the scaled row
		{
			memcpy ((uint8_t *) pDst + r * s_nHdmiPitch, pDst, w * s * 2);
		}
	}
}

void display_draw (int x, int y, int w, int h, const uint16_t *data)
{
	if (s_pHdmi)
	{
		hdmi_draw (x, y, w, h, data);
	}
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
