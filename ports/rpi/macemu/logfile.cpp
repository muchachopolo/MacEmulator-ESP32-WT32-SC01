#include "logfile.h"
#include <fatfs/ff.h>
#include <circle/util.h>

#define LOG_FILE	"SD:/mac/log.txt"

CLogFile::CLogFile (void)
:	m_pTarget (0),
	m_nUsed (0),
	m_nSerial (0),
	m_nSaved (0)
{
}

int CLogFile::Write (const void *pBuffer, size_t nCount)
{
	if (m_pTarget)
	{
		m_pTarget->Write (pBuffer, nCount);
	}
	m_Lock.Acquire ();
	if (nCount > BufferSize / 2)
	{
		nCount = BufferSize / 2;
	}
	if (m_nUsed + nCount > BufferSize)		// full: keep the newest half
	{
		unsigned nKeep = BufferSize / 2;
		memmove (m_Buffer, m_Buffer + m_nUsed - nKeep, nKeep);
		m_nUsed = nKeep;
	}
	memcpy (m_Buffer + m_nUsed, pBuffer, nCount);
	m_nUsed += nCount;
	m_nSerial++;
	m_Lock.Release ();
	return (int) nCount;
}

void CLogFile::Save (boolean bForce)
{
	if (!bForce && m_nSaved == m_nSerial)
	{
		return;
	}
	m_nSaved = m_nSerial;
	static char s_Copy[BufferSize];			// other cores keep logging meanwhile
	m_Lock.Acquire ();
	unsigned nUsed = m_nUsed;
	memcpy (s_Copy, m_Buffer, nUsed);
	m_Lock.Release ();
	FIL File;
	if (f_open (&File, LOG_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		return;
	}
	UINT nWritten;
	f_write (&File, s_Copy, nUsed, &nWritten);
	f_close (&File);
}
