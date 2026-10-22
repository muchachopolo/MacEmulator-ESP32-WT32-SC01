// SD worker: core 2 does all SD card access once the Mac runs, so the
// emulator (core 0) never stops for the card except when it needs a part of a
// disk that is not in RAM yet. In priority order it serves load requests
// from core 0, writes changed disk blocks back (after the Mac has stopped
// writing for a second), saves the log (log=1, every 30 s) and prefetches the
// rest of the disk images into RAM. FatFs is not thread-safe: before Start()
// everything runs inline on core 0, afterwards only on core 2.
#ifndef _sdworker_h
#define _sdworker_h

#include <circle/types.h>
#include <circle/synchronize.h>

class CDiskImage;
class CLogFile;

class CSDWorker
{
public:
	CSDWorker (void);
	static CSDWorker *Get (void) { return s_pThis; }

	void SetImages (CDiskImage *pDisk1, CDiskImage *pDisk2) { m_pDisk[0] = pDisk1; m_pDisk[1] = pDisk2; }
	void SetLog (CLogFile *pLog, boolean bEnabled) { m_pLog = pLog; m_bLog = bEnabled; }

	// Core 0, just before the other cores start: from now on every file
	// access goes through the worker.
	void Start (void) { DataMemBarrier (); m_bRunning = TRUE; }

	void Run (void);				// core 2, never returns

	// Core 0: load a range now (waits for the worker, or loads inline
	// before the worker runs).
	void Load (CDiskImage *pImage, unsigned nOffset, unsigned nCount);

	// Core 0: write everything out and wait (before a restart).
	void SyncAll (void);

private:
	void FlushImages (boolean bForce);

	CDiskImage *m_pDisk[2];
	CLogFile *m_pLog;
	boolean m_bLog;
	volatile boolean m_bRunning;

	// one load request at a time (core 0 waits for it)
	CDiskImage *volatile m_pReqImage;
	volatile unsigned m_nReqOffset, m_nReqCount;
	volatile unsigned m_nReqSeq, m_nDoneSeq;
	volatile unsigned m_nSyncSeq, m_nSyncDoneSeq;

	static CSDWorker *s_pThis;
};

#endif
