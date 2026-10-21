#
# R1 display test for the Raspberry Pi 2 + AC-TFT35 (ILI9486 via 16-bit shift
# registers). Bare metal, built with Circle (https://github.com/rsta2/circle,
# GPL-3.0; fetched separately, not part of this repository).
#
# CIRCLEHOME must point at a Circle checkout configured with RASPPI = 2 and
# built (./makeall --nosample). Default: ~/tools/circle.
#
CIRCLEHOME ?= $(HOME)/tools/circle

OBJS	= main.o kernel.o ili9486.o

LIBS	= $(CIRCLEHOME)/lib/libcircle.a

include $(CIRCLEHOME)/Rules.mk

-include $(DEPS)
