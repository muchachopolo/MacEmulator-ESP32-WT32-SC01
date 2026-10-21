# Host (x86 Linux/WSL) build of the ESP32 Macintosh emulator core.
# Compiles main/mac_*.c and components/musashi UNMODIFIED against the
# ESP-IDF shims in host/shim/.
#
#   make            -> build/macemu
#   make run        -> 3000 frames, screenshots every 300, RAM dump
#   make M32=1      -> 32-bit build (closer to ESP32 type sizes; needs gcc-multilib)

ROOT     := ..
ROM_DIR  ?= $(abspath $(ROOT)/../ROM)
BUILD    ?= build$(if $(TRACE),_trace,)
CC       ?= gcc

# Mirror main/CMakeLists.txt: ROM_NAME "Color-Classic.ROM" -> MAC_ROM_LC_II=1,
# HAS_DISK_IMAGE=1 when OS_750_boot.dsk exists.
MAC_ROM_LC_II  ?= 1
# MAC_V8_CUDA: 1 = Color Classic (bring-up ROM, Cuda), 0 = LC II (target, Egret)
MAC_V8_CUDA    ?= 0
HAS_DISK_IMAGE ?= $(if $(wildcard $(ROM_DIR)/OS_750_boot.dsk),1,0)
ifeq ($(MAC_ROM_LC_II),1)
ifeq ($(MAC_V8_CUDA),1)
ROM_FILE := Color-Classic.ROM
ROM_SYM  := Color_Classic_ROM
else
ROM_FILE := LC-II.ROM
ROM_SYM  := LC_II_ROM
endif
else
ROM_FILE := Classic.ROM
ROM_SYM  := Classic_ROM
endif

ARCH     := $(if $(M32),-m32,)
OPT      ?= -O2 -g
DEFS     := -DHAS_DISK_IMAGE=$(HAS_DISK_IMAGE) -DMAC_ROM_LC_II=$(MAC_ROM_LC_II) -DMAC_V8_CUDA=$(MAC_V8_CUDA) -DHOST_BUILD=1 $(if $(TRACE),-DHOST_TRACE=1,) $(EXTRA_DEFS)
INCS     := -Ishim -I$(ROOT)/main -I$(ROOT)/components/musashi -I$(ROOT)/components/display/include
CFLAGS   := $(ARCH) $(OPT) -std=gnu17 -fno-strict-aliasing $(DEFS) $(INCS)
WARN     := -Wall -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable
# Same suppressions as components/musashi/CMakeLists.txt
MUSASHI_WARN := -w

CORE_SRCS := mac_emulator.c mac_memory.c mac_video.c mac_via.c mac_scc.c mac_scsi.c mac_iwm.c mac_egret.c mac_v8video.c mac_v8disk.c mac_touch.c
MUSASHI_SRCS := m68kcpu.c m68kops.c softfloat/softfloat.c

OBJS := $(addprefix $(BUILD)/main/,$(CORE_SRCS:.c=.o)) \
        $(addprefix $(BUILD)/musashi/,$(MUSASHI_SRCS:.c=.o)) \
        $(BUILD)/host_main.o $(BUILD)/host_prof.o $(BUILD)/display_stub.o $(BUILD)/rom_blobs.o

SHIMS := $(wildcard shim/*.h shim/freertos/*.h)

all: $(BUILD)/macemu

$(BUILD)/macemu: $(OBJS)
	$(CC) $(ARCH) -o $@ $^ -lm

$(BUILD)/main/%.o: $(ROOT)/main/%.c $(wildcard $(ROOT)/main/*.h) $(SHIMS) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WARN) -c $< -o $@

$(BUILD)/musashi/%.o: $(ROOT)/components/musashi/%.c $(wildcard $(ROOT)/components/musashi/*.h) $(ROOT)/components/musashi/m68kfpu.c $(SHIMS) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(MUSASHI_WARN) -c $< -o $@

$(BUILD)/%.o: %.c $(SHIMS) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WARN) -c $< -o $@

$(BUILD)/rom_blobs.o: rom_blobs.S $(ROM_DIR)/$(ROM_FILE) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(ARCH) $(DEFS) -DROM_FILE='"$(ROM_FILE)"' -DROM_SYM=$(ROM_SYM) \
	    -Wa,-I$(ROM_DIR) -c $< -o $@

FRAMES ?= 3000
run: $(BUILD)/macemu
	mkdir -p out
	./$(BUILD)/macemu --frames $(FRAMES) --shot-every 300 --out out \
	    --dump-ram out/ram.bin --log out/emu.log

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
