#include "sdworker.h"
#include "diskimage.h"
#include "logfile.h"
#include <circle/timer.h>
#include <circle/synchronize.h>
#include <circle/logger.h>

static const char From[] = "sd";
static const u64 FlushQuietUs = 1000000;	// write back 1 s after the last write
static const u64 LogSaveUs = 30000000;

CSDWorker *CSDWorker::s_pThis = 0;

CSDWorker::CSDWorker (void)
:	m_pLog (0),
	m_bLog (FALSE),
	m_bRunning (FALSE),
	m_pReqImage (0),
	m_nReqOffset (0),
	m_nReqCount (0),
	m_nReqSeq (0),
	m_nDoneSeq (0),
	m_nSyncSeq (0),
	m_nSyncDoneSeq (0)
{
	m_pDisk[0] = m_pDisk[1] = 0;
	s_pThis = this;
}

void CSDWorker::Load (CDiskImage *pImage, unsigned nOffset, unsigned nCount)
{
	if (!m_bRunning)
	{
		pImage->Load (nOffset, nCount);
		return;
	}
	m_pReqImage = pImage;
	m_nReqOffset = nOffset;
	m_nReqCount = nCount;
	DataMemBarrier ();
	unsigned nSeq = m_nReqSeq + 1;
	m_nReqSeq = nSeq;
	while (m_nDoneSeq != nSeq)
	{
		// spin: the worker answers within one SD read
	}
	DataMemBarrier ();
}

void CSDWorker::SyncAll (void)
{
	if (!m_bRunning)
	{
		FlushImages (TRUE);
		return;
	}
	unsigned nSeq = m_nSyncSeq + 1;
	m_nSyncSeq = nSeq;
	u64 nStart = CTimer::GetClockTicks64 ();
	while (m_nSyncDoneSeq != nSeq && CTimer::GetClockTicks64 () - nStart < 5000000)
	{
	}
}

void CSDWorker::FlushImages (boolean bForce)
{
	u64 nNow = CTimer::GetClockTicks64 ();
	for (unsigned i = 0; i < 2; i++)
	{
		CDiskImage *p = m_pDisk[i];
		if (p && p->HasDirty () && (bForce || nNow - p->GetLastWrite () >= FlushQuietUs))
		{
			unsigned n = p->FlushDirty ();
			if (m_bLog)
			{
				CLogger::Get ()->Write (From, LogNotice, "%s: saved %u blocks", p->GetName (), n);
			}
		}
	}
}

void CSDWorker::Run (void)
{
	DataMemBarrier ();
	u64 nLastLog = CTimer::GetClockTicks64 ();
	for (;;)
	{
		if (m_nReqSeq != m_nDoneSeq)			// core 0 is waiting
		{
			DataMemBarrier ();
			m_pReqImage->Load (m_nReqOffset, m_nReqCount);
			DataMemBarrier ();
			m_nDoneSeq = m_nReqSeq;
			continue;
		}
		if (m_nSyncSeq != m_nSyncDoneSeq)
		{
			unsigned nSeq = m_nSyncSeq;
			FlushImages (TRUE);
			if (m_bLog && m_pLog)
			{
				m_pLog->Save (TRUE);
			}
			m_nSyncDoneSeq = nSeq;
			continue;
		}
		FlushImages (FALSE);
		u64 nNow = CTimer::GetClockTicks64 ();
		if (m_bLog && m_pLog && nNow - nLastLog >= LogSaveUs)
		{
			nLastLog = nNow;
			m_pLog->Save ();
		}
		boolean bBusy = FALSE;
		for (unsigned i = 0; i < 2 && !bBusy; i++)	// one piece, then re-check requests
		{
			bBusy = m_pDisk[i] && m_pDisk[i]->PrefetchStep ();
		}
		if (!bBusy)
		{
			CTimer::SimpleusDelay (1000);		// idle
		}
	}
}
