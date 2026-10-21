// R3: the unmodified emulator core (MacintoshESPEmulator/main + Musashi) on a
// bare-metal Raspberry Pi 2, single core: the 68k runs in mac_emu_run() and the
// per-frame hook (platform.cpp) refreshes the LCD and samples touch.
#include "kernel.h"
#include "mac_emulator.h"
#include "display.h"

static const char From[] = "r3";

CKernel *CKernel::s_pThis = 0;

CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_CPUThrottle (CPUSpeedMaximum),
	m_SPI (CILI9486::MaxClock, 0, 0),
	m_LCD (&m_SPI),
	m_Touch (&m_SPI)
{
	s_pThis = this;
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
	return bOK;
}

TShutdownMode CKernel::Run (void)
{
	m_Logger.Write (From, LogNotice, "R3 emulator, compiled " __DATE__ " " __TIME__);
	m_Logger.Write (From, LogNotice, "ARM clock %u MHz", m_CPUThrottle.GetClockRate () / 1000000);

	if (!display_init () || !mac_emu_init ())
	{
		m_Logger.Write (From, LogPanic, "emulator init failed");
		return ShutdownHalt;
	}
	mac_emu_run (0);					// never returns

	return ShutdownHalt;
}
