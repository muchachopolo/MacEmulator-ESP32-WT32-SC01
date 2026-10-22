// Core 1 of the Pi 2 drives the AC-TFT35: it renders changed strips of the Mac
// screen (mac_video_render -> display_draw -> ILI9486) and samples the XPT2046,
// like the ESP32 build's core-0 video and touch tasks. Core 0 runs the 68k and
// owns everything else (SD card, logging, interrupts). Only core 1 uses SPI0.
#ifndef _videocore_h
#define _videocore_h

#include <circle/multicore.h>
#include <circle/memory.h>
#include <circle/types.h>

class CVideoCore : public CMultiCoreSupport
{
public:
	CVideoCore (CMemorySystem *pMemorySystem);

	void Run (unsigned nCore);

	// Sample the touch panel if the last sample is older than ~16 ms
	// (called from the render loop and between strips).
	static void PollTouch (void);

	// Latest sample, in the WT32-SC01 FT5x06 orientation the core expects.
	static boolean GetTouch (int *pX, int *pY);

	static volatile unsigned s_nDraws;		// display_draw() calls (render progress)

private:
	static volatile u32 s_nTouch;			// bit 31 pressed, x << 16 | y
};

#endif
