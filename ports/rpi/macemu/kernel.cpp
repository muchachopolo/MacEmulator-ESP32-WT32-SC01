// The emulator core (MacintoshESPEmulator/main + Musashi) on a bare-metal
// Raspberry Pi 2: core 0 runs the 68k in mac_emu_run() (its per-frame hook in
// platform.cpp saves the disk), core 1 refreshes the LCD and samples touch
// (videocore.cpp).
// R5: the ROM and the disk image are files on the SD card (/mac/), and blocks
// the Mac writes are written back to the disk image file.
#include "kernel.h"
#include "mac_emulator.h"
#include "mac_disk_overlay.h"
#include "display.h"
#include <circle/util.h>
#include <circle/alloc.h>

static const char From[] = "mac";

#define DRIVE		"SD:"
#define MAC_DIR		DRIVE "/mac/"
#if MAC_ROM_LC_II
#define ROM_NAME	"LC-II.ROM"
#else
#define ROM_NAME	"Classic.ROM"
#endif
static const char *const DefaultDisks[] = { "custom_753.dsk", "OS_750_boot.dsk" };

// Write-back: flush once the Mac has not written for this long (a file save is
// a burst of writes), and at most this often while it keeps writing.
static const u64 FlushQuietUs = 1000000;
static const u64 FlushMaxDelayUs = 10000000;

// Error codes shown on the LCD (red screen + number)
enum { ErrorSDCard = 1, ErrorROM = 2, ErrorInit = 3 };

CKernel *CKernel::s_pThis = 0;

CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_CPUThrottle (CPUSpeedMaximum),
	m_SPI (CILI9486::MaxClock, 0, 0),
	m_LCD (&m_SPI),
	m_Touch (&m_SPI),
	m_EMMC (&m_Interrupt, &m_Timer, &m_ActLED),
	m_VideoCore (CMemorySystem::Get ()),
	m_USBHCI (&m_Interrupt, &m_Timer, TRUE),		// TRUE: plug-and-play
	m_USBInput (&m_USBHCI),
	m_pDisk (0),
	m_nDiskSize (0),
	m_nFlushedWrites (0),
	m_nLastWriteSeen (0)
{
	s_pThis = this;
	m_DiskName[0] = 0;
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
	if (bOK) bOK = m_EMMC.Initialize ();
	if (bOK) bOK = m_USBHCI.Initialize ();
	return bOK;
}

void CKernel::ShowError (unsigned nCode)
{
	char Text[4] = { (char) ('0' + nCode), 0 };
	m_LCD.Fill (0, 0, CILI9486::Width, CILI9486::Height, 0xF800);
	m_LCD.DrawDigits (CILI9486::Width / 2 - 24, CILI9486::Height / 2 - 40, 16, Text, 0xFFFF);
}

u8 *CKernel::LoadFile (const char *pName, unsigned *pSize)
{
	FIL File;
	if (f_open (&File, pName, FA_READ | FA_OPEN_EXISTING) != FR_OK)
	{
		return 0;
	}
	unsigned nSize = f_size (&File);
	u8 *pBuffer = (u8 *) malloc (nSize ? nSize : 1);
	UINT nRead = 0;
	if (!pBuffer || f_read (&File, pBuffer, nSize, &nRead) != FR_OK || nRead != nSize)
	{
		free (pBuffer);
		f_close (&File);
		m_Logger.Write (From, LogError, "%s: read failed", pName);
		return 0;
	}
	f_close (&File);
	*pSize = nSize;
	m_Logger.Write (From, LogNotice, "%s: %u bytes", pName, nSize);
	return pBuffer;
}

// /mac/config.txt: "disk=<file in /mac>" (one setting per line, # comments).
void CKernel::ReadConfig (void)
{
	unsigned nSize;
	char *pText = (char *) LoadFile (MAC_DIR "config.txt", &nSize);
	if (!pText)
	{
		return;
	}
	for (unsigned i = 0; i < nSize; )
	{
		unsigned nEnd = i;
		while (nEnd < nSize && pText[nEnd] != '\n')
		{
			nEnd++;
		}
		unsigned nLen = nEnd - i;
		while (nLen && (pText[i + nLen - 1] == '\r' || pText[i + nLen - 1] == ' '))
		{
			nLen--;
		}
		if (nLen > 5 && strncmp (&pText[i], "disk=", 5) == 0 && nLen - 5 < sizeof m_DiskName)
		{
			memcpy (m_DiskName, &pText[i + 5], nLen - 5);
			m_DiskName[nLen - 5] = 0;
		}
		i = nEnd + 1;
	}
	free (pText);
}

boolean CKernel::LoadImages (void)
{
	if (f_mount (&m_FileSystem, DRIVE, 1) != FR_OK)
	{
		m_Logger.Write (From, LogError, "cannot mount the SD card");
		ShowError (ErrorSDCard);
		return FALSE;
	}

	unsigned nROMSize;
	u8 *pROM = LoadFile (MAC_DIR ROM_NAME, &nROMSize);
	if (!pROM)
	{
		m_Logger.Write (From, LogError, "missing " MAC_DIR ROM_NAME);
		ShowError (ErrorROM);
		return FALSE;
	}
	mac_ext_rom = pROM;
	mac_ext_rom_size = nROMSize;

	ReadConfig ();
	for (unsigned i = 0; !m_pDisk && i <= sizeof DefaultDisks / sizeof DefaultDisks[0]; i++)
	{
		if (i > 0)
		{
			strcpy (m_DiskName, DefaultDisks[i - 1]);
		}
		if (m_DiskName[0])
		{
			CString Path;
			Path.Format (MAC_DIR "%s", m_DiskName);
			m_pDisk = LoadFile (Path, &m_nDiskSize);
		}
	}
	if (!m_pDisk)
	{
		m_Logger.Write (From, LogWarning, "no disk image: the Mac will ask for a disk");
		m_DiskName[0] = 0;
		return TRUE;
	}

	// The whole image fits the overlay, so every block can be written; the
	// file is updated from it by FlushDisk().
	mac_ext_disk = m_pDisk;
	mac_ext_disk_size = m_nDiskSize;
	disk_ov_pool_kb = (m_nDiskSize + 1023) / 1024;
	return TRUE;
}

void CKernel::FlushDisk (boolean bForce)
{
	if (!m_pDisk || !disk_ov_active ())
	{
		return;
	}
	unsigned nWrites = disk_ov_write_count ();
	if (nWrites == m_nFlushedWrites)
	{
		return;
	}
	u64 nNow = CTimer::GetClockTicks64 ();
	static unsigned s_nSeen;
	static u64 s_nFirstPending;
	if (nWrites != s_nSeen)
	{
		if (s_nSeen == m_nFlushedWrites)
		{
			s_nFirstPending = nNow;			// first write since the last flush
		}
		s_nSeen = nWrites;
		m_nLastWriteSeen = nNow;
	}
	if (!bForce && nNow - m_nLastWriteSeen < FlushQuietUs
	    && nNow - s_nFirstPending < FlushMaxDelayUs)
	{
		return;
	}

	CString Path;
	Path.Format (MAC_DIR "%s", m_DiskName);
	FIL File;
	if (f_open (&File, Path, FA_WRITE | FA_OPEN_EXISTING) != FR_OK)
	{
		m_Logger.Write (From, LogError, "%s: cannot open for writing", (const char *) Path);
		m_nFlushedWrites = nWrites;			// don't retry every frame
		return;
	}
	unsigned nBlocks = 0;
	u8 Block[DISK_OV_BLOCK];
	for (unsigned nOffset = 0; nOffset < m_nDiskSize; nOffset += DISK_OV_BLOCK)
	{
		unsigned nLen = m_nDiskSize - nOffset < DISK_OV_BLOCK ? m_nDiskSize - nOffset : DISK_OV_BLOCK;
		disk_ov_read (Block, nOffset, nLen);
		if (memcmp (Block, m_pDisk + nOffset, nLen) == 0)
		{
			continue;
		}
		UINT nWritten = 0;
		if (f_lseek (&File, nOffset) != FR_OK
		    || f_write (&File, Block, nLen, &nWritten) != FR_OK || nWritten != nLen)
		{
			m_Logger.Write (From, LogError, "%s: write failed at %u", (const char *) Path, nOffset);
			break;
		}
		memcpy (m_pDisk + nOffset, Block, nLen);	// the file now holds it
		nBlocks++;
	}
	f_close (&File);
	m_nFlushedWrites = nWrites;
	s_nSeen = nWrites;
	m_Logger.Write (From, LogNotice, "%s: saved %u blocks", (const char *) Path, nBlocks);
}

TShutdownMode CKernel::Run (void)
{
	m_Logger.Write (From, LogNotice, "Mac emulator, compiled " __DATE__ " " __TIME__);
	m_Logger.Write (From, LogNotice, "ARM clock %u MHz", m_CPUThrottle.GetClockRate () / 1000000);

	display_init ();
	if (!LoadImages ())
	{
		return ShutdownHalt;
	}
	if (!mac_emu_init ())
	{
		m_Logger.Write (From, LogPanic, "emulator init failed");
		ShowError (ErrorInit);
		return ShutdownHalt;
	}
	if (!m_VideoCore.Initialize ())				// starts core 1
	{
		m_Logger.Write (From, LogPanic, "cannot start core 1");
		return ShutdownHalt;
	}
	mac_emu_run (0);					// never returns

	return ShutdownHalt;
}
