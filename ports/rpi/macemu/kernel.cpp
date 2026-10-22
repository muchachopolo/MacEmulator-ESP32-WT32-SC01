// The emulator core (MacintoshESPEmulator/main + Musashi) on a bare-metal
// Raspberry Pi 2: core 0 runs the 68k in mac_emu_run(), core 1 refreshes the
// LCD and samples touch (videocore.cpp), core 2 does all SD card access once
// the Mac runs (sdworker.cpp). The ROM and the disk images are files on the SD
// card (/mac/); disks are loaded into RAM on demand and in the background, and
// blocks the Mac writes are written back to the files.
#include "kernel.h"
#include "mac_emulator.h"
#include "mac_disk_overlay.h"
#include "mac_v8disk.h"
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

// Copy the overlay's changed blocks into the drive-1 image at most this often
// (RAM only; the SD worker writes them to the card a second later).
static const u64 SyncIntervalUs = 250000;

// Error codes shown on the LCD (red screen + number)
enum { ErrorSDCard = 1, ErrorROM = 2, ErrorInit = 3 };

CKernel *CKernel::s_pThis = 0;

// Large f_read()s straight into heap buffers sometimes never return in
// Circle's SD (SDHOST) driver; reads in 64 KB pieces through a static buffer
// have been reliable. Every file read goes through here.
static u8 s_Bounce[0x10000] __attribute__ ((aligned (64)));

FRESULT ReadPieces (FIL *pFile, u8 *pDest, unsigned nCount)
{
	for (unsigned nDone = 0; nDone < nCount; )
	{
		unsigned nPiece = nCount - nDone < sizeof s_Bounce ? nCount - nDone : sizeof s_Bounce;
		UINT nGot = 0;
		FRESULT Res = f_read (pFile, s_Bounce, nPiece, &nGot);
		if (Res != FR_OK)
		{
			return Res;
		}
		if (nGot != nPiece)
		{
			return FR_INT_ERR;
		}
		memcpy (pDest + nDone, s_Bounce, nPiece);
		nDone += nPiece;
	}
	return FR_OK;
}

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
	m_nSyncedWrites (0),
	m_nLastSync (0),
	m_bMounted (FALSE)
{
	s_pThis = this;
	m_bLog = FALSE;
	m_DiskName[0] = 0;
	m_Disk2Name[0] = 0;
	m_ActLED.Blink (3);
}

boolean CKernel::Initialize (void)
{
	boolean bOK = m_Screen.Initialize ();
	if (bOK) bOK = m_Serial.Initialize (115200);
	if (bOK)
	{
		CDevice *pTarget = m_DeviceNameService.GetDevice (m_Options.GetLogDevice (), FALSE);
		m_LogFile.SetTarget (pTarget ? pTarget : &m_Screen);
		bOK = m_Logger.Initialize (&m_LogFile);
		m_Logger.RegisterPanicHandler (PanicHandler);
	}
	if (bOK) bOK = m_Interrupt.Initialize ();
	if (bOK) bOK = m_Timer.Initialize ();
	if (bOK) bOK = m_SPI.Initialize ();
	if (bOK) bOK = m_EMMC.Initialize ();
	if (bOK) bOK = m_USBHCI.Initialize ();
	return bOK;
}

// A panic (exception, assertion): save the log, show a red 9.
void CKernel::PanicHandler (void)
{
	if (s_pThis->m_bMounted)
	{
		s_pThis->m_LogFile.Save (TRUE);
	}
	s_pThis->ShowError (9);
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
	if (!pBuffer)
	{
		f_close (&File);
		m_Logger.Write (From, LogError, "%s: no memory for %u bytes", pName, nSize);
		return 0;
	}
	// Read in 1 MB pieces; files over 4 MB (big disk images) get a progress
	// bar, since 500 MB take a while to load from the card.
	const unsigned nChunk = 0x100000;
	const unsigned W = CILI9486::Width, H = CILI9486::Height;
	boolean bBar = nSize > 4 * nChunk;
	if (bBar)
	{
		m_LCD.Fill (40, H / 2 - 12, W - 80, 24, 0x8410);	// grey frame
	}
	u64 nStart = CTimer::GetClockTicks64 ();
	for (unsigned nDone = 0; nDone < nSize; )
	{
		unsigned n = nSize - nDone < nChunk ? nSize - nDone : nChunk;
		if (ReadPieces (&File, pBuffer + nDone, n) != FR_OK)
		{
			free (pBuffer);
			f_close (&File);
			m_Logger.Write (From, LogError, "%s: read failed at %u", pName, nDone);
			return 0;
		}
		nDone += n;
		if (bBar)
		{
			unsigned nWidth = (unsigned) ((u64) (W - 84) * nDone / nSize);
			m_LCD.Fill (42, H / 2 - 10, nWidth, 20, 0x07E0);	// green
		}
	}
	if (bBar)
	{
		unsigned nMs = (unsigned) ((CTimer::GetClockTicks64 () - nStart) / 1000);
		m_Logger.Write (From, LogNotice, "%s: loaded in %u ms", pName, nMs);
		m_LCD.Fill (0, 0, W, H, 0);
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
		else if (nLen == 5 && strncmp (&pText[i], "log=", 4) == 0)
		{
			m_bLog = pText[i + 4] == '1';
		}
		else if (nLen > 6 && strncmp (&pText[i], "disk2=", 6) == 0 && nLen - 6 < sizeof m_Disk2Name)
		{
			memcpy (m_Disk2Name, &pText[i + 6], nLen - 6);
			m_Disk2Name[nLen - 6] = 0;
		}
		i = nEnd + 1;
	}
	free (pText);
}

// disk_ov_fill: the overlay is about to read drive-1 bytes
void CKernel::DiskFill (uint32_t nOffset, uint32_t nCount)
{
	s_pThis->m_Disk1.Ensure (nOffset, nCount);
}

boolean CKernel::LoadImages (void)
{
	if (f_mount (&m_FileSystem, DRIVE, 1) != FR_OK)
	{
		m_Logger.Write (From, LogError, "cannot mount the SD card");
		ShowError (ErrorSDCard);
		return FALSE;
	}
	m_bMounted = TRUE;

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
	m_Logger.Write (From, LogNotice, "disk setting: \"%s\", log %s", m_DiskName, m_bLog ? "on" : "off");
	if (m_bLog)
	{
		m_LogFile.Save (TRUE);
	}
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
			if (m_Disk1.Open (Path))
			{
				m_pDisk = m_Disk1.GetData ();
				m_nDiskSize = m_Disk1.GetSize ();
				m_Disk1.Load (0, 1);		// boot blocks + MDB
				disk_ov_fill = DiskFill;
			}
		}
	}
	if (!m_pDisk)
	{
		m_Logger.Write (From, LogWarning, "no disk image: the Mac will ask for a disk");
		m_DiskName[0] = 0;
		return TRUE;
	}

#if MAC_ROM_LC_II && MAC_DRIVE2
	if (m_Disk2Name[0])				// second disk (drive 2)
	{
		CString Path;
		Path.Format (MAC_DIR "%s", m_Disk2Name);
		if (m_Disk2.Open (Path))
		{
			v8disk_set_drive2 (m_Disk2.GetSize (), Drive2IO, TRUE);
		}
		else
		{
			m_Logger.Write (From, LogWarning, "%s: cannot open", (const char *) Path);
		}
	}
#endif

	// The whole image fits the overlay, so every block can be written; the
	// file is updated from it by FlushDisk().
	mac_ext_disk = m_pDisk;
	mac_ext_disk_size = m_nDiskSize;
	disk_ov_pool_kb = (m_nDiskSize + 1023) / 1024;
	return TRUE;
}

int CKernel::Drive2IO (bool bWrite, uint8_t *pBuffer, uint32_t nOffset, uint32_t nCount)
{
	return bWrite ? s_pThis->m_Disk2.Write (pBuffer, nOffset, nCount)
		      : s_pThis->m_Disk2.Read (pBuffer, nOffset, nCount);
}

// Drive 1 goes through the core's copy-on-write overlay: copy the blocks the
// Mac changed into the image (RAM) and mark them dirty; the SD worker writes
// them to the card. Cheap (no SD access), called from the frame hook.
void CKernel::FlushDisk (boolean bForce)
{
	if (!m_pDisk || !disk_ov_active ())
	{
		return;
	}
	unsigned nWrites = disk_ov_write_count ();
	u64 nNow = CTimer::GetClockTicks64 ();
	if (nWrites == m_nSyncedWrites || (!bForce && nNow - m_nLastSync < SyncIntervalUs))
	{
		return;
	}
	m_nSyncedWrites = nWrites;
	m_nLastSync = nNow;
	u8 Block[DISK_OV_BLOCK];
	for (int32_t nBlock = disk_ov_next_written (0); nBlock >= 0;
	     nBlock = disk_ov_next_written (nBlock + 1))
	{
		unsigned nOffset = (unsigned) nBlock * DISK_OV_BLOCK;
		unsigned nLen = m_nDiskSize - nOffset < DISK_OV_BLOCK ? m_nDiskSize - nOffset : DISK_OV_BLOCK;
		disk_ov_read (Block, nOffset, nLen);		// loaded: it was written
		if (memcmp (Block, m_pDisk + nOffset, nLen) != 0)
		{
			memcpy (m_pDisk + nOffset, Block, nLen);
			m_Disk1.MarkDirty (nOffset, nLen);
		}
	}
}

TShutdownMode CKernel::Run (void)
{
	m_Logger.Write (From, LogNotice, "Mac emulator, compiled " __DATE__ " " __TIME__);
	m_Logger.Write (From, LogNotice, "ARM clock %u MHz, memory %u MB", m_CPUThrottle.GetClockRate () / 1000000,
			(unsigned) (CMemorySystem::Get ()->GetMemSize () >> 20));

	display_init ();
	if (!LoadImages ())
	{
		if (m_bMounted) m_LogFile.Save (TRUE);
		return ShutdownHalt;
	}
	if (m_bLog)
	{
		m_LogFile.Save (TRUE);
	}
	if (!mac_emu_init ())
	{
		m_Logger.Write (From, LogError, "emulator init failed");
		m_LogFile.Save (TRUE);
		ShowError (ErrorInit);
		return ShutdownHalt;
	}
	if (m_bLog)
	{
		m_LogFile.Save (TRUE);
	}
	m_SDWorker.SetImages (m_Disk1.IsOpen () ? &m_Disk1 : 0, m_Disk2.IsOpen () ? &m_Disk2 : 0);
	m_SDWorker.SetLog (&m_LogFile, m_bLog);
	m_SDWorker.Start ();
	if (!m_VideoCore.Initialize ())				// starts cores 1 and 2
	{
		m_Logger.Write (From, LogPanic, "cannot start core 1");
		return ShutdownHalt;
	}
	mac_emu_run (0);					// never returns

	return ShutdownHalt;
}
