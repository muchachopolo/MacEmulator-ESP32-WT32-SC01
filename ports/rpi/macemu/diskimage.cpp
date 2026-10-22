#include "diskimage.h"
#include "sdworker.h"
#include <circle/alloc.h>
#include <circle/util.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/synchronize.h>

static const char From[] = "disk";
static const unsigned ChunkShift = 20;			// 1 MB
static const unsigned Piece = 0x10000;			// 64 KB per SD read
static const int ioErr = -36;

FRESULT ReadPieces (FIL *pFile, u8 *pDest, unsigned nCount);	// kernel.cpp

CDiskImage::CDiskImage (void)
:	m_pData (0),
	m_nSize (0),
	m_pChunkLoaded (0),
	m_nChunks (0),
	m_pDirty (0),
	m_bDirty (FALSE),
	m_nLastWrite (0),
	m_nPrefetchChunk (0),
	m_nPrefetchPiece (0),
	m_bAllLoaded (FALSE)
{
	m_Name[0] = 0;
}

boolean CDiskImage::Open (const char *pPath)
{
	if (f_open (&m_File, pPath, FA_READ | FA_WRITE | FA_OPEN_EXISTING) != FR_OK)
	{
		return FALSE;
	}
	strncpy (m_Name, pPath, sizeof m_Name - 1);
	m_Name[sizeof m_Name - 1] = 0;
	m_nSize = f_size (&m_File);
	m_nChunks = (m_nSize + (1u << ChunkShift) - 1) >> ChunkShift;
	m_pData = (u8 *) malloc (m_nSize ? m_nSize : 1);
	m_pChunkLoaded = (u8 *) calloc (m_nChunks ? m_nChunks : 1, 1);
	m_pDirty = (u8 *) calloc (m_nSize / 512 + 1, 1);
	if (!m_pData || !m_pChunkLoaded || !m_pDirty)
	{
		free (m_pData);
		free ((void *) m_pChunkLoaded);
		free ((void *) m_pDirty);
		m_pData = 0;
		f_close (&m_File);
		CLogger::Get ()->Write (From, LogError, "%s: no memory for %u bytes", pPath, m_nSize);
		return FALSE;
	}
	CLogger::Get ()->Write (From, LogNotice, "%s: %u bytes, loaded on demand", pPath, m_nSize);
	return TRUE;
}

boolean CDiskImage::IsLoaded (unsigned nOffset, unsigned nCount) const
{
	if (m_bAllLoaded || !nCount)
	{
		return TRUE;
	}
	unsigned nLast = (nOffset + nCount - 1) >> ChunkShift;
	for (unsigned c = nOffset >> ChunkShift; c <= nLast && c < m_nChunks; c++)
	{
		if (!m_pChunkLoaded[c])
		{
			return FALSE;
		}
	}
	return TRUE;
}

void CDiskImage::Ensure (unsigned nOffset, unsigned nCount)
{
	if (!IsLoaded (nOffset, nCount))
	{
		CSDWorker::Get ()->Load (this, nOffset, nCount);
	}
}

boolean CDiskImage::Load (unsigned nOffset, unsigned nCount)
{
	if (!nCount)
	{
		return TRUE;
	}
	unsigned nLast = (nOffset + nCount - 1) >> ChunkShift;
	for (unsigned c = nOffset >> ChunkShift; c <= nLast && c < m_nChunks; c++)
	{
		if (m_pChunkLoaded[c])
		{
			continue;
		}
		unsigned nPos = c << ChunkShift;
		unsigned n = m_nSize - nPos < (1u << ChunkShift) ? m_nSize - nPos : 1u << ChunkShift;
		if (   f_lseek (&m_File, nPos) != FR_OK
		    || ReadPieces (&m_File, m_pData + nPos, n) != FR_OK)
		{
			CLogger::Get ()->Write (From, LogError, "read failed at %u", nPos);
			return FALSE;
		}
		DataMemBarrier ();				// data before the flag
		m_pChunkLoaded[c] = 1;
	}
	return TRUE;
}

boolean CDiskImage::PrefetchStep (void)
{
	const unsigned nPieces = (1u << ChunkShift) / Piece;
	while (m_nPrefetchChunk < m_nChunks && m_pChunkLoaded[m_nPrefetchChunk])
	{
		m_nPrefetchChunk++;				// loaded meanwhile (on demand)
		m_nPrefetchPiece = 0;
	}
	if (!m_pData || m_nPrefetchChunk >= m_nChunks)
	{
		if (m_pData && !m_bAllLoaded)
		{
			m_bAllLoaded = TRUE;
			CLogger::Get ()->Write (From, LogNotice, "%s: all in RAM", m_Name);
		}
		return FALSE;
	}
	unsigned nPos = (m_nPrefetchChunk << ChunkShift) + m_nPrefetchPiece * Piece;
	unsigned n = nPos < m_nSize ? (m_nSize - nPos < Piece ? m_nSize - nPos : Piece) : 0;
	if (n && (f_lseek (&m_File, nPos) != FR_OK || ReadPieces (&m_File, m_pData + nPos, n) != FR_OK))
	{
		CLogger::Get ()->Write (From, LogError, "prefetch failed at %u", nPos);
		m_nPrefetchChunk = m_nChunks;			// give up; on demand still works
		return FALSE;
	}
	if (++m_nPrefetchPiece >= nPieces || nPos + n >= m_nSize)
	{
		DataMemBarrier ();
		m_pChunkLoaded[m_nPrefetchChunk++] = 1;
		m_nPrefetchPiece = 0;
	}
	return TRUE;
}

int CDiskImage::Read (u8 *pBuffer, unsigned nOffset, unsigned nCount)
{
	Ensure (nOffset, nCount);
	if (!IsLoaded (nOffset, nCount))
	{
		return ioErr;
	}
	memcpy (pBuffer, m_pData + nOffset, nCount);
	return 0;
}

int CDiskImage::Write (const u8 *pBuffer, unsigned nOffset, unsigned nCount)
{
	Ensure (nOffset, nCount);			// partial blocks keep their rest
	if (!IsLoaded (nOffset, nCount))
	{
		return ioErr;
	}
	memcpy (m_pData + nOffset, pBuffer, nCount);
	MarkDirty (nOffset, nCount);
	return 0;
}

void CDiskImage::MarkDirty (unsigned nOffset, unsigned nCount)
{
	DataMemBarrier ();				// data before the flags
	for (unsigned b = nOffset / 512; b * 512 < nOffset + nCount; b++)
	{
		m_pDirty[b] = 1;
	}
	m_nLastWrite = CTimer::GetClockTicks64 ();
	m_bDirty = TRUE;
}

unsigned CDiskImage::FlushDirty (void)
{
	m_bDirty = FALSE;				// a write from now on sets it again
	DataMemBarrier ();
	unsigned nWritten = 0;
	unsigned nBlocks = m_nSize / 512;
	for (unsigned b = 0; b < nBlocks; )
	{
		if (!m_pDirty[b])
		{
			b++;
			continue;
		}
		unsigned e = b;					// a run of dirty blocks; clear the
		while (e < nBlocks && m_pDirty[e] && e - b < 128)	// flags before copying
		{
			m_pDirty[e++] = 0;
		}
		DataMemBarrier ();
		UINT nDone = 0;
		if (   f_lseek (&m_File, b * 512) != FR_OK
		    || f_write (&m_File, m_pData + b * 512, (e - b) * 512, &nDone) != FR_OK)
		{
			CLogger::Get ()->Write (From, LogError, "%s: write failed at %u", m_Name, b * 512);
		}
		nWritten += e - b;
		b = e;
	}
	f_sync (&m_File);
	return nWritten;
}
