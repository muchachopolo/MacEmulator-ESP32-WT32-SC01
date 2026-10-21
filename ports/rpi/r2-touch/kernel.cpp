// R2: XPT2046 touch on the AC-TFT35. Touch four targets to calibrate, then
// draw with the finger/stylus; the top line shows raw X, Y and pressure.
#include "kernel.h"
#include <circle/util.h>

static const char From[] = "r2";

#define RGB565(r, g, b)	((u16) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define BLACK		RGB565 (0, 0, 0)
#define WHITE		RGB565 (255, 255, 255)
#define RED		RGB565 (255, 0, 0)
#define GREEN		RGB565 (0, 255, 0)
#define YELLOW		RGB565 (255, 255, 0)
#define CYAN		RGB565 (0, 255, 255)
#define GREY		RGB565 (64, 64, 64)

// Targets: top-left, top-right, bottom-left, bottom-right.
static const unsigned TargetX0 = 40, TargetX1 = CILI9486::Width - 40;
static const unsigned TargetY0 = 40, TargetY1 = CILI9486::Height - 40;
static const unsigned TargetX[4] = { TargetX0, TargetX1, TargetX0, TargetX1 };
static const unsigned TargetY[4] = { TargetY0, TargetY0, TargetY1, TargetY1 };

static const unsigned InfoHeight = 24;			// raw-value line at the top
static const unsigned ClearBox = 40;			// bottom-left: tap to clear

CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_SPI (CILI9486::MaxClock, 0, 0),
	m_LCD (&m_SPI),
	m_Touch (&m_SPI),
	m_bSwap (FALSE),
	m_nU0 (0), m_nU1 (4095), m_nV0 (0), m_nV1 (4095)
{
	m_ActLED.Blink (3);
}

boolean CKernel::Initialize (void)
{
	boolean bOK = m_Screen.Initialize ();
	if (bOK) bOK = m_Serial.Initialize (115200);
	if (bOK)
	{
		CDevice *pTarget = m_DeviceNameService.GetDevice (m_Options.GetLogDevice (), FALSE);
		bOK = m_Logger.Initialize (pTarget ? pTarget : &m_Screen);
	}
	if (bOK) bOK = m_Interrupt.Initialize ();
	if (bOK) bOK = m_Timer.Initialize ();
	if (bOK) bOK = m_SPI.Initialize ();
	return bOK;
}

void CKernel::DrawTarget (unsigned x, unsigned y, u16 nColor)
{
	m_LCD.Fill (x - 15, y, 31, 1, nColor);
	m_LCD.Fill (x, y - 15, 1, 31, nColor);
	m_LCD.Fill (x - 3, y - 3, 7, 7, nColor);
}

// Wait for a firm press, average it while held, wait for the release.
void CKernel::CaptureTarget (unsigned nIndex, unsigned *pX, unsigned *pY)
{
	for (;;)
	{
		DrawTarget (TargetX[nIndex], TargetY[nIndex], WHITE);
		char Text[4] = { (char) ('1' + nIndex), 0 };
		m_LCD.DrawDigits (CILI9486::Width / 2 - 12, CILI9486::Height / 2 - 30, 12, Text, YELLOW, BLACK);

		while (!m_Touch.IsPressed ())
		{
			CTimer::SimpleMsDelay (5);
		}
		CTimer::SimpleMsDelay (50);			// let the contact settle

		unsigned nSumX = 0, nSumY = 0, nCount = 0, x, y, z;
		for (unsigned i = 0; i < 40; i++)		// ~0.4 s of samples
		{
			if (m_Touch.Read (&x, &y, &z))
			{
				nSumX += x;
				nSumY += y;
				nCount++;
			}
			CTimer::SimpleMsDelay (10);
		}

		unsigned nReleased = 0;
		while (nReleased < 20)				// released for 100 ms
		{
			nReleased = m_Touch.IsPressed () ? 0 : nReleased + 1;
			CTimer::SimpleMsDelay (5);
		}

		if (nCount >= 20)
		{
			*pX = nSumX / nCount;
			*pY = nSumY / nCount;
			DrawTarget (TargetX[nIndex], TargetY[nIndex], GREEN);
			m_Logger.Write (From, LogNotice, "target %u: raw %u %u (%u samples)",
					nIndex + 1, *pX, *pY, nCount);
			return;
		}
		DrawTarget (TargetX[nIndex], TargetY[nIndex], RED);	// too short: again
		CTimer::SimpleMsDelay (300);
	}
}

void CKernel::Calibrate (void)
{
	unsigned a[4], b[4];
	m_LCD.Fill (0, 0, CILI9486::Width, CILI9486::Height, BLACK);
	for (unsigned i = 0; i < 4; i++)
	{
		CaptureTarget (i, &a[i], &b[i]);
	}

	// Which raw axis follows screen x? Compare right minus left targets.
	int dA = (int) (a[1] + a[3]) - (int) (a[0] + a[2]);
	int dB = (int) (b[1] + b[3]) - (int) (b[0] + b[2]);
	m_bSwap = (dB < 0 ? -dB : dB) > (dA < 0 ? -dA : dA);
	const unsigned *u = m_bSwap ? b : a, *v = m_bSwap ? a : b;
	m_nU0 = (int) (u[0] + u[2]) / 2;			// left
	m_nU1 = (int) (u[1] + u[3]) / 2;			// right
	m_nV0 = (int) (v[0] + v[1]) / 2;			// top
	m_nV1 = (int) (v[2] + v[3]) / 2;			// bottom
	if (m_nU1 == m_nU0) m_nU1++;
	if (m_nV1 == m_nV0) m_nV1++;

	m_Logger.Write (From, LogNotice, "calibration: swap %u, x %d..%d, y %d..%d",
			m_bSwap, m_nU0, m_nU1, m_nV0, m_nV1);

	// Show the result for the photo: swap, left, right, top, bottom.
	m_LCD.Fill (0, 0, CILI9486::Width, CILI9486::Height, BLACK);
	unsigned Values[5] = { m_bSwap, (unsigned) m_nU0, (unsigned) m_nU1,
			       (unsigned) m_nV0, (unsigned) m_nV1 };
	for (unsigned i = 0; i < 5; i++)
	{
		CString s;
		s.Format ("%u", Values[i]);
		m_LCD.DrawDigits (40, 20 + i * 58, 8, s, i ? CYAN : YELLOW);
	}
	CTimer::SimpleMsDelay (6000);
}

boolean CKernel::Map (unsigned nRawX, unsigned nRawY, unsigned *pX, unsigned *pY)
{
	int u = m_bSwap ? nRawY : nRawX, v = m_bSwap ? nRawX : nRawY;
	int x = (int) TargetX0 + (u - m_nU0) * (int) (TargetX1 - TargetX0) / (m_nU1 - m_nU0);
	int y = (int) TargetY0 + (v - m_nV0) * (int) (TargetY1 - TargetY0) / (m_nV1 - m_nV0);
	if (x < 0 || y < 0 || x >= (int) CILI9486::Width || y >= (int) CILI9486::Height)
	{
		return FALSE;
	}
	*pX = x;
	*pY = y;
	return TRUE;
}

void CKernel::ShowNumbers (unsigned y, const unsigned *pValues, unsigned nCount, u16 nColor)
{
	for (unsigned i = 0; i < nCount; i++)
	{
		CString s;
		s.Format ("%4u", pValues[i]);			// fixed width; spaces = blank cells
		m_LCD.DrawDigits (10 + i * 120, y, 4, s, nColor, BLACK);
	}
}

void CKernel::PaintScreen (void)
{
	m_LCD.Fill (0, 0, CILI9486::Width, CILI9486::Height, BLACK);
	m_LCD.Fill (0, InfoHeight, CILI9486::Width, 1, GREY);
	m_LCD.Fill (0, CILI9486::Height - ClearBox, ClearBox, ClearBox, RED);
	for (unsigned i = 0; i < 4; i++)				// accuracy check targets
	{
		DrawTarget (TargetX[i], TargetY[i], GREY);
	}
}

TShutdownMode CKernel::Run (void)
{
	m_Logger.Write (From, LogNotice, "R2 touch test, compiled " __DATE__ " " __TIME__);
	m_LCD.SetClock (CILI9486::MaxClock);
	m_LCD.Initialize ();

	Calibrate ();
	PaintScreen ();

	unsigned nLastX = 0, nLastY = 0;
	boolean bWasDown = FALSE;
	for (;;)
	{
		unsigned rx, ry, rz, x, y;
		if (!m_Touch.Read (&rx, &ry, &rz))
		{
			bWasDown = FALSE;
			CTimer::SimpleMsDelay (5);
			continue;
		}

		unsigned Raw[3] = { rx, ry, rz };
		ShowNumbers (2, Raw, 3, WHITE);
		if (!Map (rx, ry, &x, &y))
		{
			continue;
		}
		if (x < ClearBox && y >= CILI9486::Height - ClearBox)
		{
			PaintScreen ();
			while (m_Touch.IsPressed ())
			{
				CTimer::SimpleMsDelay (5);
			}
			bWasDown = FALSE;
			continue;
		}

		// Join successive samples with a line so fast strokes stay continuous.
		unsigned nSteps = 1;
		if (bWasDown)
		{
			int dx = (int) x - (int) nLastX, dy = (int) y - (int) nLastY;
			unsigned d = (unsigned) ((dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
						 ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy));
			nSteps = d ? d : 1;
		}
		for (unsigned s = 1; s <= nSteps; s++)
		{
			int px = bWasDown ? (int) nLastX + ((int) x - (int) nLastX) * (int) s / (int) nSteps : (int) x;
			int py = bWasDown ? (int) nLastY + ((int) y - (int) nLastY) * (int) s / (int) nSteps : (int) y;
			if (py >= (int) InfoHeight + 2 && px >= 1 && px + 2 <= (int) CILI9486::Width)
			{
				m_LCD.Fill (px - 1, py - 1, 3, 3, GREEN);
			}
		}
		nLastX = x;
		nLastY = y;
		bWasDown = TRUE;
	}

	return ShutdownHalt;
}
