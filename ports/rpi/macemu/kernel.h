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
#include "usbinput.h"
#include "logfile.h"
#include "diskimage.h"
#include "sdworker.h"

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
	CUSBInput *GetUSBInput (void) { return &m_USBInput; }
	CLogFile *GetLogFile (void) { return &m_LogFile; }
	boolean IsLogEnabled (void) const { return m_bLog; }	// /mac/config.txt log=1

	// Copy the blocks the Mac changed on drive 1 into its image (RAM; the
	// SD worker writes them). bForce: now, not at most every 250 ms.
	void FlushDisk (boolean bForce);
	CSDWorker *GetSDWorker (void) { return &m_SDWorker; }

private:
	boolean LoadImages (void);
	u8 *LoadFile (const char *pName, unsigned *pSize);
	void ReadConfig (void);
	void ShowError (unsigned nCode);
	static void PanicHandler (void);
	static void DiskFill (uint32_t nOffset, uint32_t nCount);	// disk_ov_fill
	static int Drive2IO (bool bWrite, uint8_t *pBuffer, uint32_t nOffset, uint32_t nCount);

	// do not change this order
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CScreenDevice		m_Screen;		// HDMI log (optional)
	CSerialDevice		m_Serial;		// UART log on GPIO14/15 (optional)
	CLogFile		m_LogFile;		// log tee -> SD:/mac/log.txt
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
	CUSBHCIDevice		m_USBHCI;
	CUSBInput		m_USBInput;

	boolean		m_bLog;				// /mac/config.txt "log=1"
	char		m_DiskName[64];			// /mac/config.txt "disk="
	u8		*m_pDisk;			// drive-1 image data (m_Disk1)
	unsigned	m_nDiskSize;
	unsigned	m_nSyncedWrites;		// disk_ov_write_count() at the last sync
	u64		m_nLastSync;
	boolean		m_bMounted;
	CDiskImage	m_Disk1;
	char		m_Disk2Name[64];		// /mac/config.txt "disk2=" (LC II)
	CDiskImage	m_Disk2;
	CSDWorker	m_SDWorker;			// core 2: all SD access after start

	static CKernel *s_pThis;
};

#endif
