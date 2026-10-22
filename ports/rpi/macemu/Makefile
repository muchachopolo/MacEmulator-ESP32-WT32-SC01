#
# The emulator core (MacintoshESPEmulator/main + Musashi, unmodified) on a
# bare-metal Raspberry Pi 2 with the AC-TFT35. Built with Circle
# (https://github.com/rsta2/circle, GPL-3.0; fetched separately) and the Arm GNU
# toolchain's newlib (vsnprintf, setjmp, libm).
#
#   make                  Mac Classic -> kernel7.img      (boots SD:/mac/Classic.ROM)
#   make MAC_ROM_LC_II=1  LC II       -> kernel7-lc2.img  (boots SD:/mac/LC-II.ROM)
#   make all-machines     both
#
# Pick the machine on the SD card with "kernel=" in /config.txt. The disk image
# is SD:/mac/<disk= in SD:/mac/config.txt>, else custom_753.dsk, else
# OS_750_boot.dsk; changes the Mac makes are saved back to that file.
#
CIRCLEHOME ?= $(HOME)/tools/circle
EMU        := ../../../MacintoshESPEmulator

MAC_ROM_LC_II ?= 0
ifeq ($(MAC_ROM_LC_II),1)
MACHINE    := lc2
TARGET     := kernel7-lc2
else
MACHINE    := classic
TARGET     := kernel7
endif
O          := obj-$(MACHINE)

CORE_SRCS    := mac_emulator.c mac_memory.c mac_video.c mac_via.c mac_scc.c mac_scsi.c \
                mac_iwm.c mac_egret.c mac_v8video.c mac_v8disk.c mac_touch.c mac_rtc.c \
                mac_disk_overlay.c mac_kbd.c
MUSASHI_SRCS := m68kcpu.c m68kops.c softfloat/softfloat.c mac_native.c
PI_SRCS      := main.cpp kernel.cpp platform.cpp videocore.cpp usbinput.cpp
COMMON_SRCS  := ili9486.cpp xpt2046.cpp

OBJS	= $(addprefix $(O)/,$(PI_SRCS:.cpp=.o)) $(O)/syscalls.o \
	  $(addprefix $(O)/common/,$(COMMON_SRCS:.cpp=.o)) \
	  $(addprefix $(O)/main/,$(CORE_SRCS:.c=.o)) \
	  $(addprefix $(O)/musashi/,$(MUSASHI_SRCS:.c=.o))

# HOST_BUILD=1 selects the non-ESP-IDF paths of the core (native-order RGB565,
# inline touch polling, no FreeRTOS); the Pi keeps the real-time throttle and
# supplies the ROM and disk at run time (MAC_EXTERNAL_IMAGES) and a USB keyboard
# (MAC_KEYBOARD).
EMU_DEFS = -DHOST_BUILD=1 -DMAC_FRAME_THROTTLE=1 -DMAC_EXTERNAL_IMAGES=1 -DHAS_DISK_IMAGE=1 \
	   -DMAC_KEYBOARD=1 \
	   -DMAC_ROM_LC_II=$(MAC_ROM_LC_II) -DMAC_V8_CUDA=0 $(EXTRA_DEFS)

EXTRAINCLUDE = -I shim -I ../common -I $(EMU)/main -I $(EMU)/components/musashi \
	       -I $(EMU)/components/display/include

LIBS	= $(CIRCLEHOME)/addon/fatfs/libfatfs.a $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
	  $(CIRCLEHOME)/lib/usb/libusb.a $(CIRCLEHOME)/lib/input/libinput.a \
	  $(CIRCLEHOME)/lib/fs/libfs.a $(CIRCLEHOME)/lib/libcircle.a

CHECK_DEPS = 0
include $(CIRCLEHOME)/Rules.mk

LIBC	= "$(shell $(PREFIX)gcc $(ARCHCPU) -print-file-name=libc.a)"
EXTRALIBS += $(LIBC)

EMU_WARN = -Wno-format -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable

$(O)/%.o: %.cpp Makefile
	@mkdir -p $(dir $@)
	@echo "  CPP   $@"
	@$(CPP) $(CPPFLAGS) $(EMU_DEFS) -MMD -c -o $@ $<

$(O)/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	@echo "  CC    $@"
	@$(CC) $(CFLAGS) $(EMU_DEFS) -MMD -c -o $@ $<

$(O)/common/%.o: ../common/%.cpp Makefile
	@mkdir -p $(dir $@)
	@echo "  CPP   $@"
	@$(CPP) $(CPPFLAGS) -MMD -c -o $@ $<

$(O)/main/%.o: $(EMU)/main/%.c Makefile
	@mkdir -p $(dir $@)
	@echo "  CC    $@"
	@$(CC) $(CFLAGS) $(EMU_DEFS) -std=gnu17 -fno-strict-aliasing $(EMU_WARN) -MMD -c -o $@ $<

$(O)/musashi/%.o: $(EMU)/components/musashi/%.c Makefile
	@mkdir -p $(dir $@)
	@echo "  CC    $@"
	@$(CC) $(CFLAGS) $(EMU_DEFS) -std=gnu17 -fno-strict-aliasing -w -MMD -c -o $@ $<

all-machines:
	$(MAKE) MAC_ROM_LC_II=0
	$(MAKE) MAC_ROM_LC_II=1

clean-emu:
	rm -rf obj-classic obj-lc2 kernel7*.img kernel7*.elf kernel7*.lst kernel7*.map

-include $(OBJS:.o=.d)
