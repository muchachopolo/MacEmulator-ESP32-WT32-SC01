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
#include <circle/cputhrottle.h>
#include <circle/spimaster.h>
#include <circle/types.h>
#include "ili9486.h"
#include "xpt2046.h"

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

	// Platform services for the C emulator core (platform.cpp).
	static CKernel *Get (void) { return s_pThis; }
	CLogger *GetLogger (void) { return &m_Logger; }
	CILI9486 *GetLCD (void) { return &m_LCD; }
	CXPT2046 *GetTouch (void) { return &m_Touch; }
	CCPUThrottle *GetCPUThrottle (void) { return &m_CPUThrottle; }

private:
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
	CCPUThrottle		m_CPUThrottle;
	CSPIMaster		m_SPI;
	CILI9486		m_LCD;
	CXPT2046		m_Touch;

	static CKernel *s_pThis;
};

#endif
