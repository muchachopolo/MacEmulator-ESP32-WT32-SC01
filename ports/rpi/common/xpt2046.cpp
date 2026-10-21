#include "xpt2046.h"
#include <circle/timer.h>

// Control byte: S | A2 A1 A0 | MODE (0 = 12 bit) | SER/DFR (0 = differential) |
// PD1 PD0 (00 = power down between conversions, PENIRQ enabled).
static const u8 CmdX  = 0xD0;			// A = 101
static const u8 CmdY  = 0x90;			// A = 001
static const u8 CmdZ1 = 0xB0;			// A = 011
static const u8 CmdZ2 = 0xC0;			// A = 100

static const unsigned Samples = 7;

CXPT2046::CXPT2046 (CSPIMaster *pSPI, unsigned nChipSelect, unsigned nIrqPin, unsigned nClockHz)
:	m_pSPI (pSPI),
	m_nCS (nChipSelect),
	m_Irq (nIrqPin, GPIOModeInputPullUp),
	m_nClockHz (nClockHz)
{
}

boolean CXPT2046::IsPressed (void)
{
	return m_Irq.Read () == LOW;
}

unsigned CXPT2046::Convert (u8 nCommand)
{
	u8 Tx[3] = { nCommand, 0, 0 };
	u8 Rx[3] = { 0, 0, 0 };
	m_pSPI->WriteRead (m_nCS, Tx, Rx, sizeof Tx);
	return (((unsigned) Rx[1] << 8) | Rx[2]) >> 3 & 0xFFF;	// 12 bits after the busy bit
}

unsigned CXPT2046::Median (u8 nCommand)
{
	unsigned v[Samples];
	Convert (nCommand);				// first conversion settles the input
	for (unsigned i = 0; i < Samples; i++)
	{
		unsigned x = Convert (nCommand), j = i;
		for (; j > 0 && v[j - 1] > x; j--)	// insertion sort
		{
			v[j] = v[j - 1];
		}
		v[j] = x;
	}
	return (v[Samples / 2 - 1] + v[Samples / 2] + v[Samples / 2 + 1]) / 3;
}

boolean CXPT2046::Read (unsigned *pX, unsigned *pY, unsigned *pZ)
{
	if (!IsPressed ())
	{
		return FALSE;
	}
	m_pSPI->SetClock (m_nClockHz);
	unsigned x = Median (CmdX);
	unsigned y = Median (CmdY);
	unsigned z1 = Median (CmdZ1);
	unsigned z2 = Median (CmdZ2);
	CTimer::SimpleusDelay (20);			// PENIRQ settles after the last conversion
	if (!IsPressed ())				// lifted during the read: discard
	{
		return FALSE;
	}
	*pX = x;
	*pY = y;
	*pZ = z1 + 4095 - z2;
	return TRUE;
}
