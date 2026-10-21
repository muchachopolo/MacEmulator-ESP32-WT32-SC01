// ILI9486 480x320 on the "3.5inch RPi LCD (A)" design (AC-TFT35-V2.1 and
// clones): the controller sits behind two 74HC4094 shift registers and a
// 74HC4040 counter, so every SPI transfer is a 16-bit word (commands and
// parameters are sent zero-extended to 16 bits, pixels as RGB565) and the bus
// is write-only. Pins: SPI0 CE0, DC = GPIO24, RESET = GPIO25.
#ifndef _ili9486_h
#define _ili9486_h

#include <circle/spimaster.h>
#include <circle/gpiopin.h>
#include <circle/types.h>

class CILI9486
{
public:
	static const unsigned Width  = 480;
	static const unsigned Height = 320;

	CILI9486 (CSPIMaster *pSPI, unsigned nChipSelect = 0,
		  unsigned nDCPin = 24, unsigned nResetPin = 25);

	void Initialize (void);
	void SetClock (unsigned nHz);				// commands and pixels
	// Commands/parameters at nCmdHz, pixel data at nDataHz. Short transfers
	// lose their last word above ~18 MHz on this board; long pixel streams don't.
	void SetClocks (unsigned nCmdHz, unsigned nDataHz);

	// Fill a rectangle with one RGB565 colour.
	void Fill (unsigned x, unsigned y, unsigned w, unsigned h, u16 nColor);
	// Write a w*h block of RGB565 pixels (row-major).
	void Blit (unsigned x, unsigned y, unsigned w, unsigned h, const u16 *pPixels);

private:
	void Command (u8 nCmd);
	void Data (const u8 *pData, unsigned nCount);
	void SetWindow (unsigned x0, unsigned y0, unsigned x1, unsigned y1);
	void SendWords (const u16 *pWords, unsigned nCount);	// big-endian on the wire

	CSPIMaster *m_pSPI;
	unsigned    m_nCS;
	unsigned    m_nCmdHz;
	unsigned    m_nDataHz;
	CGPIOPin    m_DC;
	CGPIOPin    m_Reset;
	u8          m_Buf[2 * Width * 20];			// 20 lines of RGB565
};

#endif
