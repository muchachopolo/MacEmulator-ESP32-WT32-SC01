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
#include <SDCard/emmc.h>
#include <fatfs/ff.h>
#include "ili9486.h"
#include "xpt2046.h"
#include "videocore.h"

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

	// Write the blocks the Mac changed back to the disk image file.
	// bForce: also when the last write is recent (restart, shutdown).
	void FlushDisk (boolean bForce);

private:
	boolean LoadImages (void);
	u8 *LoadFile (const char *pName, unsigned *pSize);
	void ReadConfig (void);
	void ShowError (unsigned nCode);

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
	CEMMCDevice		m_EMMC;
	FATFS			m_FileSystem;
	CVideoCore		m_VideoCore;

	char		m_DiskName[64];			// /mac/config.txt "disk="
	u8		*m_pDisk;			// pristine copy = what the file holds
	unsigned	m_nDiskSize;
	unsigned	m_nFlushedWrites;		// disk_ov_write_count() at the last flush
	u64		m_nLastWriteSeen;		// when that count last changed

	static CKernel *s_pThis;
};

#endif
