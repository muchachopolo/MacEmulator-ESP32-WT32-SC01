#include "diskimage.h"
#include <circle/alloc.h>
#include <circle/util.h>
#include <circle/logger.h>

static const char From[] = "disk";
static const unsigned ChunkShift = 20;			// 1 MB
static const int ioErr = -36;

FRESULT ReadPieces (FIL *pFile, u8 *pDest, unsigned nCount);	// kernel.cpp

CDiskImage::CDiskImage (void)
:	m_pData (0),
	m_nSize (0),
	m_pChunkLoaded (0),
	m_nChunks (0),
	m_pDirty (0),
	m_nDirty (0),
	m_nPrefetchChunk (0),
	m_nPrefetchPiece (0)
{
}

boolean CDiskImage::Open (const char *pPath)
{
	if (f_open (&m_File, pPath, FA_READ | FA_WRITE | FA_OPEN_EXISTING) != FR_OK)
	{
		return FALSE;
	}
	m_nSize = f_size (&m_File);
	m_nChunks = (m_nSize + (1u << ChunkShift) - 1) >> ChunkShift;
	m_pData = (u8 *) malloc (m_nSize ? m_nSize : 1);
	m_pChunkLoaded = (u8 *) calloc (m_nChunks ? m_nChunks : 1, 1);
	m_pDirty = (u8 *) calloc ((m_nSize / 512 + 8) / 8, 1);
	if (!m_pData || !m_pChunkLoaded || !m_pDirty)
	{
		free (m_pData);
		free (m_pChunkLoaded);
		free (m_pDirty);
		m_pData = 0;
		f_close (&m_File);
		CLogger::Get ()->Write (From, LogError, "%s: no memory for %u bytes", pPath, m_nSize);
		return FALSE;
	}
	CLogger::Get ()->Write (From, LogNotice, "%s: %u bytes, loaded on demand", pPath, m_nSize);
	return TRUE;
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
		m_pChunkLoaded[c] = 1;
	}
	return TRUE;
}

int CDiskImage::Read (u8 *pBuffer, unsigned nOffset, unsigned nCount)
{
	if (!Load (nOffset, nCount))
	{
		return ioErr;
	}
	memcpy (pBuffer, m_pData + nOffset, nCount);
	return 0;
}

int CDiskImage::Write (const u8 *pBuffer, unsigned nOffset, unsigned nCount)
{
	if (!Load (nOffset, nCount))			// partial blocks keep their rest
	{
		return ioErr;
	}
	memcpy (m_pData + nOffset, pBuffer, nCount);
	for (unsigned b = nOffset / 512; b * 512 < nOffset + nCount; b++)
	{
		if (!(m_pDirty[b / 8] & (1 << (b % 8))))
		{
			m_pDirty[b / 8] |= 1 << (b % 8);
			m_nDirty++;
		}
	}
	return 0;
}

unsigned CDiskImage::Flush (void)
{
	unsigned nWritten = 0;
	unsigned nBlocks = m_nSize / 512;
	for (unsigned b = 0; b < nBlocks && m_nDirty; )
	{
		if (!(m_pDirty[b / 8] & (1 << (b % 8))))
		{
			b++;
			continue;
		}
		unsigned e = b;					// a run of dirty blocks
		while (e < nBlocks && (m_pDirty[e / 8] & (1 << (e % 8))))
		{
			m_pDirty[e / 8] &= ~(1 << (e % 8));
			m_nDirty--;
			e++;
		}
		UINT nDone = 0;
		if (   f_lseek (&m_File, b * 512) != FR_OK
		    || f_write (&m_File, m_pData + b * 512, (e - b) * 512, &nDone) != FR_OK)
		{
			CLogger::Get ()->Write (From, LogError, "write failed at %u", b * 512);
		}
		nWritten += e - b;
		b = e;
	}
	f_sync (&m_File);
	return nWritten;
}

boolean CDiskImage::PrefetchStep (void)
{
	const unsigned nPiece = 0x10000, nPieces = (1u << ChunkShift) / nPiece;
	while (m_nPrefetchChunk < m_nChunks && m_pChunkLoaded[m_nPrefetchChunk])
	{
		m_nPrefetchChunk++;				// loaded meanwhile (on demand)
		m_nPrefetchPiece = 0;
	}
	if (!m_pData || m_nPrefetchChunk >= m_nChunks)
	{
		return FALSE;
	}
	unsigned nPos = (m_nPrefetchChunk << ChunkShift) + m_nPrefetchPiece * nPiece;
	unsigned n = nPos < m_nSize ? (m_nSize - nPos < nPiece ? m_nSize - nPos : nPiece) : 0;
	if (n && (f_lseek (&m_File, nPos) != FR_OK || ReadPieces (&m_File, m_pData + nPos, n) != FR_OK))
	{
		CLogger::Get ()->Write (From, LogError, "prefetch failed at %u", nPos);
		m_nPrefetchChunk = m_nChunks;			// give up; on-demand still works
		return FALSE;
	}
	if (++m_nPrefetchPiece >= nPieces || nPos + n >= m_nSize)
	{
		m_pChunkLoaded[m_nPrefetchChunk++] = 1;
		m_nPrefetchPiece = 0;
	}
	return TRUE;
}
