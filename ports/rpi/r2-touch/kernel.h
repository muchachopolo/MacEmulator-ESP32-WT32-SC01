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

private:
	void DrawTarget (unsigned x, unsigned y, u16 nColor);
	void CaptureTarget (unsigned nIndex, unsigned *pX, unsigned *pY);
	void Calibrate (void);
	boolean Map (unsigned nRawX, unsigned nRawY, unsigned *pX, unsigned *pY);
	void ShowNumbers (unsigned y, const unsigned *pValues, unsigned nCount, u16 nColor);
	void PaintScreen (void);

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
	CXPT2046		m_Touch;

	// Calibration: screen x comes from raw X (or raw Y if m_bSwap),
	// x = TargetX0 + (u - m_nU0) * (TargetX1 - TargetX0) / (m_nU1 - m_nU0), same for y.
	boolean	m_bSwap;
	int	m_nU0, m_nU1, m_nV0, m_nV1;
};

#endif
