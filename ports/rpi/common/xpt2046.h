// XPT2046 resistive touch controller on the AC-TFT35 ("3.5inch RPi LCD (A)"):
// SPI0 CE1, PENIRQ on GPIO17 (low while pressed). It shares SPI0 with the
// ILI9486, so every read programs its own (slow) clock; the LCD driver sets its
// clock again before each window.
#ifndef _xpt2046_h
#define _xpt2046_h

#include <circle/spimaster.h>
#include <circle/gpiopin.h>
#include <circle/types.h>

class CXPT2046
{
public:
	// The XPT2046 allows up to 2.5 MHz DCLK: 250 MHz / 128 = 1.95 MHz.
	CXPT2046 (CSPIMaster *pSPI, unsigned nChipSelect = 1, unsigned nIrqPin = 17,
		  unsigned nClockHz = 1953125);

	boolean IsPressed (void);			// PENIRQ low

	// Raw 12-bit X/Y (median of several conversions) and pressure Z
	// (higher = firmer). FALSE if the panel was not pressed for the whole read.
	boolean Read (unsigned *pX, unsigned *pY, unsigned *pZ);

private:
	unsigned Convert (u8 nCommand);
	unsigned Median (u8 nCommand);

	CSPIMaster *m_pSPI;
	unsigned    m_nCS;
	CGPIOPin    m_Irq;
	unsigned    m_nClockHz;
};

#endif
