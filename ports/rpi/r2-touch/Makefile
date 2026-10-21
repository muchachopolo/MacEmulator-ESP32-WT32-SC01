#
# R2 touch test for the Raspberry Pi 2 + AC-TFT35 (XPT2046 on SPI0 CE1,
# PENIRQ GPIO17). Bare metal, built with Circle (https://github.com/rsta2/circle,
# GPL-3.0; fetched separately, not part of this repository).
#
# CIRCLEHOME must point at a Circle checkout configured with RASPPI = 2 and
# built (./makeall --nosample). Default: ~/tools/circle.
#
CIRCLEHOME ?= $(HOME)/tools/circle

OBJS	= main.o kernel.o ../common/ili9486.o ../common/xpt2046.o

EXTRAINCLUDE = -I ../common

LIBS	= $(CIRCLEHOME)/lib/libcircle.a

include $(CIRCLEHOME)/Rules.mk

-include $(DEPS)
