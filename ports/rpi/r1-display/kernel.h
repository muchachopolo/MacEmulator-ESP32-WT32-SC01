#ifndef _kernel_h
#define _kernel_h

#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/screen.h>
#include <circle/serial.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/spimaster.h>
#include <circle/types.h>
#include "ili9486.h"

enum TShutdownMode
{
	ShutdownNone,
	ShutdownHalt,
	ShutdownReboot
};

class CKernel
{
public:
	CKernel (void);
	boolean Initialize (void);
	TShutdownMode Run (void);

private:
	void DrawNumber (unsigned x, unsigned y, unsigned nScale, const char *pText, u16 nColor);
	void DrawTestPattern (unsigned nClockIndex);

	// do not change this order
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CScreenDevice		m_Screen;		// HDMI log (optional)
	CSerialDevice		m_Serial;		// UART log on GPIO14/15 (optional)
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogger			m_Logger;
	CSPIMaster		m_SPI;
	CILI9486		m_LCD;
};

#endif
