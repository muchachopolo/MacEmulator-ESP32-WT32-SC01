// A disk image file on the SD card served from RAM: 1 MB chunks are read on
// first use (64 KB pieces through a bounce buffer), writes go to RAM and mark
// 512-byte blocks dirty, Flush() writes the dirty blocks back. Used for drive
// 2 (MAC_DRIVE2); drive 1 keeps the core's copy-on-write overlay.
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

	int Read (u8 *pBuffer, unsigned nOffset, unsigned nCount);	// 0 or Mac error
	int Write (const u8 *pBuffer, unsigned nOffset, unsigned nCount);
	boolean IsDirty (void) const { return m_nDirty != 0; }
	unsigned Flush (void);				// returns blocks written

private:
	boolean Load (unsigned nOffset, unsigned nCount);

	FIL m_File;
	u8 *m_pData;
	unsigned m_nSize;
	u8 *m_pChunkLoaded;
	unsigned m_nChunks;
	u8 *m_pDirty;					// one bit per 512-byte block
	unsigned m_nDirty;
};

#endif
