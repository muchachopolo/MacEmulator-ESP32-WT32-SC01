#
# R3: the emulator core (MacintoshESPEmulator/main + Musashi, unmodified) on a
# bare-metal Raspberry Pi 2 with the AC-TFT35. Built with Circle
# (https://github.com/rsta2/circle, GPL-3.0; fetched separately) and the Arm GNU
# toolchain's newlib (vsnprintf, setjmp, libm).
#
#   make                  Mac Classic: boots SD:/mac/Classic.ROM
#   make MAC_ROM_LC_II=1  LC II: boots SD:/mac/LC-II.ROM (make clean-emu between the two)
#
# The disk image is SD:/mac/<disk= in SD:/mac/config.txt>, else custom_753.dsk,
# else OS_750_boot.dsk; changes the Mac makes are saved back to that file.
#
CIRCLEHOME ?= $(HOME)/tools/circle
EMU        := ../../../MacintoshESPEmulator

MAC_ROM_LC_II ?= 0

CORE_SRCS    := mac_emulator.c mac_memory.c mac_video.c mac_via.c mac_scc.c mac_scsi.c \
                mac_iwm.c mac_egret.c mac_v8video.c mac_v8disk.c mac_touch.c mac_rtc.c \
                mac_disk_overlay.c
MUSASHI_SRCS := m68kcpu.c m68kops.c softfloat/softfloat.c mac_native.c

CORE_OBJS    := $(addprefix obj/main/,$(CORE_SRCS:.c=.o))
MUSASHI_OBJS := $(addprefix obj/musashi/,$(MUSASHI_SRCS:.c=.o))

OBJS	= main.o kernel.o platform.o syscalls.o ../common/ili9486.o ../common/xpt2046.o \
	  $(CORE_OBJS) $(MUSASHI_OBJS)

# HOST_BUILD=1 selects the non-ESP-IDF paths of the core (native-order RGB565,
# inline touch polling, no FreeRTOS); the Pi keeps the real-time throttle and
# supplies the ROM and disk at run time (MAC_EXTERNAL_IMAGES).
EMU_DEFS = -DHOST_BUILD=1 -DMAC_FRAME_THROTTLE=1 -DMAC_EXTERNAL_IMAGES=1 -DHAS_DISK_IMAGE=1 \
	   -DMAC_ROM_LC_II=$(MAC_ROM_LC_II) -DMAC_V8_CUDA=0 $(EXTRA_DEFS)

EXTRAINCLUDE = -I shim -I ../common -I $(EMU)/main -I $(EMU)/components/musashi \
	       -I $(EMU)/components/display/include

LIBS	= $(CIRCLEHOME)/addon/fatfs/libfatfs.a $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
	  $(CIRCLEHOME)/lib/fs/libfs.a $(CIRCLEHOME)/lib/libcircle.a

include $(CIRCLEHOME)/Rules.mk

LIBC	= "$(shell $(PREFIX)gcc $(ARCHCPU) -print-file-name=libc.a)"
EXTRALIBS += $(LIBC)

obj/main/%.o: $(EMU)/main/%.c $(wildcard $(EMU)/main/*.h) $(wildcard shim/*.h shim/freertos/*.h) Makefile
	@mkdir -p $(dir $@)
	@echo "  CC    $@"
	@$(CC) $(CFLAGS) $(EMU_DEFS) -std=gnu17 -fno-strict-aliasing -Wno-format -Wno-unused-variable \
		-Wno-unused-function -Wno-unused-but-set-variable -c -o $@ $<

obj/musashi/%.o: $(EMU)/components/musashi/%.c $(wildcard $(EMU)/components/musashi/*.h) Makefile
	@mkdir -p $(dir $@)
	@echo "  CC    $@"
	@$(CC) $(CFLAGS) $(EMU_DEFS) -std=gnu17 -fno-strict-aliasing -w -c -o $@ $<

platform.o kernel.o: CPPFLAGS += $(EMU_DEFS)

clean-emu:
	rm -rf obj

-include $(DEPS)
