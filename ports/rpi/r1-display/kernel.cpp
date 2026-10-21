// R1: bring up the AC-TFT35 (ILI9486) on a bare-metal Raspberry Pi 2, measure
// full-screen fill speed at several SPI clocks and show test patterns.
#include "kernel.h"
#include <circle/util.h>

static const char From[] = "r1";

// BCM2835/6 SPI0 clock = 250 MHz core / even divider.
// R1 result: 15.6 clean, 31.2 and above corrupt (74HC4094 chain) -> try between.
static const unsigned Clocks[] = { 15625000, 17857142, 20833333, 25000000, 31250000 };
static const char *const ClockText[] = { "15.6", "17.9", "20.8", "25.0", "31.2" };
static const unsigned NumClocks = sizeof Clocks / sizeof Clocks[0];

#define RGB565(r, g, b)	((u16) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define BLACK		RGB565 (0, 0, 0)
#define WHITE		RGB565 (255, 255, 255)
#define RED		RGB565 (255, 0, 0)
#define GREEN		RGB565 (0, 255, 0)
#define BLUE		RGB565 (0, 0, 255)
#define YELLOW		RGB565 (255, 255, 0)
#define CYAN		RGB565 (0, 255, 255)
#define MAGENTA		RGB565 (255, 0, 255)
#define GREY		RGB565 (128, 128, 128)

// 3x5 font for "0123456789." (bit 2 = left column)
static const u8 Digits[11][5] =
{
	{7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
	{7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1}, {7,5,7,5,7}, {7,5,7,1,7},
	{0,0,0,0,2},
};

CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_SPI (Clocks[0], 0, 0),
	m_LCD (&m_SPI)
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

void CKernel::DrawNumber (unsigned x, unsigned y, unsigned nScale, const char *pText, u16 nColor)
{
	for (; *pText; pText++, x += 4 * nScale)
	{
		int c = *pText == '.' ? 10 : *pText - '0';
		if (c < 0 || c > 10)
		{
			continue;
		}
		for (unsigned row = 0; row < 5; row++)
		{
			for (unsigned col = 0; col < 3; col++)
			{
				if (Digits[c][row] & (4 >> col))
				{
					m_LCD.Fill (x + col * nScale, y + row * nScale, nScale, nScale, nColor);
				}
			}
		}
	}
}

void CKernel::DrawTestPattern (unsigned nClockIndex)
{
	static const u16 Bars[8] = { WHITE, YELLOW, CYAN, GREEN, MAGENTA, RED, BLUE, BLACK };
	const unsigned W = CILI9486::Width, H = CILI9486::Height;

	for (unsigned i = 0; i < 8; i++)				// colour bars (top 2/3)
	{
		m_LCD.Fill (i * W / 8, 0, W / 8, H * 2 / 3, Bars[i]);
	}
	m_LCD.Fill (0, H * 2 / 3, W, H / 3, GREY);			// grey band
	for (unsigned x = 0; x < W; x += 40)				// grid in the band
	{
		m_LCD.Fill (x, H * 2 / 3, 1, H / 3, BLACK);
	}
	m_LCD.Fill (0, 0, W, 1, WHITE);					// 1-px border
	m_LCD.Fill (0, H - 1, W, 1, WHITE);
	m_LCD.Fill (0, 0, 1, H, WHITE);
	m_LCD.Fill (W - 1, 0, 1, H, WHITE);
	m_LCD.Fill (2, 2, 24, 24, RED);					// corner markers:
	m_LCD.Fill (W - 26, 2, 24, 24, GREEN);			//  TL red, TR green,
	m_LCD.Fill (2, H - 26, 24, 24, BLUE);			//  BL blue, BR white
	m_LCD.Fill (W - 26, H - 26, 24, 24, WHITE);
	DrawNumber (W / 2 - 60, H * 2 / 3 + 30, 8, ClockText[nClockIndex], BLACK);
}

TShutdownMode CKernel::Run (void)
{
	m_Logger.Write (From, LogNotice, "R1 display test, compiled " __DATE__ " " __TIME__);
	m_LCD.Initialize ();

	unsigned nFps10[NumClocks];
	for (unsigned i = 0; i < NumClocks; i++)
	{
		m_LCD.SetClock (Clocks[i]);
		unsigned nStart = CTimer::GetClockTicks ();
		const unsigned nFrames = 6;
		for (unsigned f = 0; f < nFrames; f++)
		{
			m_LCD.Fill (0, 0, CILI9486::Width, CILI9486::Height, (f & 1) ? BLUE : RED);
		}
		unsigned nUs = CTimer::GetClockTicks () - nStart;
		nFps10[i] = nFrames * 10000000U / (nUs ? nUs : 1);
		m_Logger.Write (From, LogNotice, "SPI %u Hz: %u us per full frame, %u.%u fps",
				Clocks[i], nUs / nFrames, nFps10[i] / 10, nFps10[i] % 10);

		DrawTestPattern (i);				// look: is this clock still clean?
		CTimer::SimpleMsDelay (4000);
	}

	// Summary at the safe clock: one bar per clock, length ~ fps, with numbers.
	m_LCD.SetClock (Clocks[0]);
	m_LCD.Fill (0, 0, CILI9486::Width, CILI9486::Height, BLACK);
	for (unsigned i = 0; i < NumClocks; i++)
	{
		unsigned y = 12 + i * 62;
		char Fps[8];
		CString s;
		s.Format ("%u.%u", nFps10[i] / 10, nFps10[i] % 10);
		strncpy (Fps, s, sizeof Fps - 1);
		Fps[sizeof Fps - 1] = 0;
		DrawNumber (10, y, 6, ClockText[i], YELLOW);
		unsigned nLen = nFps10[i] * 2;			// 0.1 fps -> 2 px
		if (nLen > 220) nLen = 220;
		m_LCD.Fill (130, y, nLen, 30, GREEN);
		DrawNumber (370, y, 6, Fps, WHITE);
	}
	m_Logger.Write (From, LogNotice, "Summary shown. Then cycling test patterns.");
	CTimer::SimpleMsDelay (8000);

	for (unsigned n = 0; ; n++)				// keep showing the pattern
	{
		DrawTestPattern (0);
		CTimer::SimpleMsDelay (5000);
		m_LCD.Fill (0, 0, CILI9486::Width, CILI9486::Height, (n & 1) ? WHITE : BLACK);
		CTimer::SimpleMsDelay (1000);
	}
	return ShutdownHalt;
}
