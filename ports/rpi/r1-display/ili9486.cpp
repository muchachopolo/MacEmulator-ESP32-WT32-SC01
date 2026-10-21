#include "ili9486.h"
#include <circle/timer.h>

// Init sequence of the Linux fbtft "waveshare35a"/"tft35a" overlay (ILI9486).
// Entries: command byte, number of parameters, parameters; 0xFF = delay (ms).
static const u8 InitSeq[] =
{
	0xB0, 1, 0x00,				// interface mode control
	0x11, 0,				// sleep out
	0xFF, 250,				// delay
	0x3A, 1, 0x55,				// pixel format: 16 bit/pixel
	0xC2, 1, 0x44,				// power control 3
	0xC5, 4, 0x00, 0x00, 0x00, 0x00,	// VCOM control
	0xE0, 15, 0x0F, 0x1F, 0x1C, 0x0C, 0x0F, 0x08, 0x48, 0x98,
		  0x37, 0x0A, 0x13, 0x04, 0x11, 0x0D, 0x00,	// positive gamma
	0xE1, 15, 0x0F, 0x32, 0x2E, 0x0B, 0x0D, 0x05, 0x47, 0x75,
		  0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00,	// negative gamma
	0xE2, 15, 0x0F, 0x32, 0x2E, 0x0B, 0x0D, 0x05, 0x47, 0x75,
		  0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00,	// digital gamma
	0x36, 1, 0x28,				// MADCTL: landscape (MV) + BGR
	0x11, 0,				// sleep out
	0x29, 0,				// display on
};

CILI9486::CILI9486 (CSPIMaster *pSPI, unsigned nChipSelect, unsigned nDCPin, unsigned nResetPin)
:	m_pSPI (pSPI),
	m_nCS (nChipSelect),
	m_DC (nDCPin, GPIOModeOutput),
	m_Reset (nResetPin, GPIOModeOutput)
{
}

void CILI9486::SetClock (unsigned nHz)
{
	m_pSPI->SetClock (nHz);
}

void CILI9486::SendWords (const u16 *pWords, unsigned nCount)
{
	// The shift registers latch 16 bits per WR strobe: send MSB first.
	unsigned nBytes = 0;
	while (nCount--)
	{
		u16 w = *pWords++;
		m_Buf[nBytes++] = w >> 8;
		m_Buf[nBytes++] = w & 0xFF;
		if (nBytes == sizeof m_Buf)
		{
			m_pSPI->Write (m_nCS, m_Buf, nBytes);
			nBytes = 0;
		}
	}
	if (nBytes)
	{
		m_pSPI->Write (m_nCS, m_Buf, nBytes);
	}
}

void CILI9486::Command (u8 nCmd)
{
	u16 w = nCmd;
	m_DC.Write (LOW);
	SendWords (&w, 1);
}

void CILI9486::Data (const u8 *pData, unsigned nCount)
{
	u16 Words[16];
	m_DC.Write (HIGH);
	while (nCount)
	{
		unsigned n = nCount < 16 ? nCount : 16;
		for (unsigned i = 0; i < n; i++)
		{
			Words[i] = pData[i];			// parameters are zero-extended
		}
		SendWords (Words, n);
		pData += n;
		nCount -= n;
	}
}

void CILI9486::Initialize (void)
{
	m_Reset.Write (HIGH);
	CTimer::SimpleMsDelay (10);
	m_Reset.Write (LOW);
	CTimer::SimpleMsDelay (20);
	m_Reset.Write (HIGH);
	CTimer::SimpleMsDelay (150);

	for (unsigned i = 0; i < sizeof InitSeq; )
	{
		u8 nCmd = InitSeq[i++];
		u8 nArg = InitSeq[i++];
		if (nCmd == 0xFF)
		{
			CTimer::SimpleMsDelay (nArg);
			continue;
		}
		Command (nCmd);
		if (nArg)
		{
			Data (&InitSeq[i], nArg);
			i += nArg;
		}
	}
	CTimer::SimpleMsDelay (50);
}

void CILI9486::SetWindow (unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
	u8 Col[4] = { (u8) (x0 >> 8), (u8) x0, (u8) (x1 >> 8), (u8) x1 };
	u8 Row[4] = { (u8) (y0 >> 8), (u8) y0, (u8) (y1 >> 8), (u8) y1 };
	Command (0x2A);
	Data (Col, 4);
	Command (0x2B);
	Data (Row, 4);
	Command (0x2C);					// memory write
	m_DC.Write (HIGH);
}

void CILI9486::Fill (unsigned x, unsigned y, unsigned w, unsigned h, u16 nColor)
{
	if (!w || !h)
	{
		return;
	}
	SetWindow (x, y, x + w - 1, y + h - 1);
	unsigned nTotal = w * h;
	unsigned nBytes = 0;
	while (nTotal--)
	{
		m_Buf[nBytes++] = nColor >> 8;
		m_Buf[nBytes++] = nColor & 0xFF;
		if (nBytes == sizeof m_Buf)
		{
			m_pSPI->Write (m_nCS, m_Buf, nBytes);
			nBytes = 0;
		}
	}
	if (nBytes)
	{
		m_pSPI->Write (m_nCS, m_Buf, nBytes);
	}
}

void CILI9486::Blit (unsigned x, unsigned y, unsigned w, unsigned h, const u16 *pPixels)
{
	SetWindow (x, y, x + w - 1, y + h - 1);
	SendWords (pPixels, w * h);
}
