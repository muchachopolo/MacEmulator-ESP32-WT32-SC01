// A disk image file on the SD card served from RAM. 1 MB chunks are read on
// first use or in the background (prefetch); written blocks are marked dirty
// and written back later. File access happens on one core only (the SD worker
// on core 2 once it runs, see sdworker.h); the emulator (core 0) only touches
// RAM and asks the worker for chunks it needs now.
#ifndef _diskimage_h
#define _diskimage_h

#include <fatfs/ff.h>
#include <circle/types.h>

class CDiskImage
{
public:
	CDiskImage (void);

	boolean Open (const char *pPath);		// FALSE: missing / no memory
	boolean IsOpen (void) const { return m_pData != 0; }
	unsigned GetSize (void) const { return m_nSize; }
	u8 *GetData (void) { return m_pData; }
	const char *GetName (void) const { return m_Name; }

	// --- emulator side (core 0) --------------------------------------------
	boolean IsLoaded (unsigned nOffset, unsigned nCount) const;
	void Ensure (unsigned nOffset, unsigned nCount);	// load now (via the worker)
	int Read (u8 *pBuffer, unsigned nOffset, unsigned nCount);	// 0 or Mac error
	int Write (const u8 *pBuffer, unsigned nOffset, unsigned nCount);	// RAM + dirty
	void MarkDirty (unsigned nOffset, unsigned nCount);	// RAM already updated
	u64 GetLastWrite (void) const { return m_nLastWrite; }

	// --- file side (SD worker, or core 0 before the worker starts) ----------
	boolean Load (unsigned nOffset, unsigned nCount);
	boolean PrefetchStep (void);			// one 64 KB piece; FALSE: all loaded
	boolean HasDirty (void) const { return m_bDirty; }
	unsigned FlushDirty (void);			// returns blocks written

private:
	FIL m_File;
	char m_Name[64];
	u8 *m_pData;
	unsigned m_nSize;
	volatile u8 *m_pChunkLoaded;			// one byte per 1 MB chunk
	unsigned m_nChunks;
	volatile u8 *m_pDirty;				// one byte per 512-byte block
	volatile boolean m_bDirty;
	volatile u64 m_nLastWrite;
	unsigned m_nPrefetchChunk, m_nPrefetchPiece;
	boolean m_bAllLoaded;
};

#endif
