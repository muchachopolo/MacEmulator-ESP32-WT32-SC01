#
# R3: the emulator core (MacintoshESPEmulator/main + Musashi, unmodified) on a
# bare-metal Raspberry Pi 2 with the AC-TFT35. Built with Circle
# (https://github.com/rsta2/circle, GPL-3.0; fetched separately) and the Arm GNU
# toolchain's newlib (vsnprintf, setjmp, libm).
#
#   make                          Classic.ROM + custom_753.dsk (falls back to OS_750_boot.dsk)
#   make MAC_ROM_LC_II=1          LC II
#
# The ROM and disk are embedded in kernel7.img (bring-up): never publish it.
#
CIRCLEHOME ?= $(HOME)/tools/circle
EMU        := ../../../MacintoshESPEmulator
ROM_DIR    ?= $(abspath ../../../ROM)

MAC_ROM_LC_II ?= 0
DISK_NAME  ?= $(if $(wildcard $(ROM_DIR)/custom_753.dsk),custom_753.dsk,OS_750_boot.dsk)
DISK_SYM   := $(shell echo '$(DISK_NAME)' | sed 's/[^A-Za-z0-9_]/_/g')
ifeq ($(MAC_ROM_LC_II),1)
ROM_FILE   := LC-II.ROM
ROM_SYM    := LC_II_ROM
else
ROM_FILE   := Classic.ROM
ROM_SYM    := Classic_ROM
endif

CORE_SRCS    := mac_emulator.c mac_memory.c mac_video.c mac_via.c mac_scc.c mac_scsi.c \
                mac_iwm.c mac_egret.c mac_v8video.c mac_v8disk.c mac_touch.c mac_rtc.c \
                mac_disk_overlay.c
MUSASHI_SRCS := m68kcpu.c m68kops.c softfloat/softfloat.c mac_native.c

CORE_OBJS    := $(addprefix obj/main/,$(CORE_SRCS:.c=.o))
MUSASHI_OBJS := $(addprefix obj/musashi/,$(MUSASHI_SRCS:.c=.o))

OBJS	= main.o kernel.o platform.o syscalls.o blobs.o ../common/ili9486.o ../common/xpt2046.o \
	  $(CORE_OBJS) $(MUSASHI_OBJS)

# HOST_BUILD=1 selects the non-ESP-IDF paths of the core (native-order RGB565,
# inline touch polling, no FreeRTOS); the Pi keeps the real-time throttle.
EMU_DEFS = -DHOST_BUILD=1 -DMAC_FRAME_THROTTLE=1 -DMAC_ROM_LC_II=$(MAC_ROM_LC_II) -DMAC_V8_CUDA=0 \
	   -DHAS_DISK_IMAGE=1 -DDISK_SYM=$(DISK_SYM) $(EXTRA_DEFS)

EXTRAINCLUDE = -I shim -I ../common -I $(EMU)/main -I $(EMU)/components/musashi \
	       -I $(EMU)/components/display/include

LIBS	= $(CIRCLEHOME)/lib/libcircle.a

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

blobs.o: blobs.S $(ROM_DIR)/$(ROM_FILE) $(ROM_DIR)/$(DISK_NAME) Makefile
	@echo "  AS    $@ ($(ROM_FILE) + $(DISK_NAME))"
	@$(CC) $(ARCHCPU) -DROM_FILE='"$(ROM_FILE)"' -DROM_SYM=$(ROM_SYM) \
		-DDISK_FILE='"$(DISK_NAME)"' -DDISK_SYM=$(DISK_SYM) -Wa,-I$(ROM_DIR) -c -o $@ $<

clean-emu:
	rm -rf obj

-include $(DEPS)
