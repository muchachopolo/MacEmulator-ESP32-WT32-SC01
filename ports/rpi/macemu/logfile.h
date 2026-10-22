// Log tee: everything Circle's logger prints (emulator ESP_LOG* lines, USB,
// exception dumps) goes to the HDMI screen as before and into a RAM buffer
// that is saved to SD:/mac/log.txt every few seconds and on a panic, so a
// failure can be diagnosed without an HDMI monitor.
#ifndef _logfile_h
#define _logfile_h

#include <circle/device.h>
#include <circle/spinlock.h>
#include <circle/types.h>

class CLogFile : public CDevice
{
public:
	CLogFile (void);

	void SetTarget (CDevice *pTarget) { m_pTarget = pTarget; }	// screen/serial

	int Write (const void *pBuffer, size_t nCount);

	// Write the buffer to the file if it changed (core 0; the file system
	// must be mounted). bForce: even if unchanged since the last save.
	void Save (boolean bForce = FALSE);

private:
	CDevice *m_pTarget;
	CSpinLock m_Lock;
	static const unsigned BufferSize = 96 * 1024;
	char m_Buffer[BufferSize];
	unsigned m_nUsed;
	unsigned m_nSerial;				// bumped on every Write
	unsigned m_nSaved;				// m_nSerial at the last save
};

#endif
