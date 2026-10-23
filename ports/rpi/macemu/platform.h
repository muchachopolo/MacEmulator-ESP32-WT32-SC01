// HDMI mirror of the Mac screen (platform.cpp).
#ifndef _platform_h
#define _platform_h

// Clear the HDMI frame buffer and start copying the Mac screen into it
// (integer-scaled, centred). Call once, before core 1 starts rendering.
void hdmi_start (void);

#endif
