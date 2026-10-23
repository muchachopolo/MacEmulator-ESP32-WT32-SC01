# Embed ROM and disk images from the ROM directory
set(ROM_DIR "${CMAKE_SOURCE_DIR}/../ROM")

# Required ROM (user-supplied, see README). Classic.ROM = Mac Classic, 68000,
# 1-bit (default); LC-II.ROM = LC II, 68030 V8 + Egret, colour;
# Color-Classic.ROM = V8 + Cuda reference build.
#set(ROM_NAME "Classic.ROM")
set(ROM_NAME "LC-II.ROM")
set(EMBED_FILES "${ROM_DIR}/${ROM_NAME}")
if(ROM_NAME STREQUAL "LC-II.ROM" OR ROM_NAME STREQUAL "Color-Classic.ROM")
    set(MAC_ROM_LC_II 1)
else()
    set(MAC_ROM_LC_II 0)
endif()
# V8 machine variant: Color Classic has Cuda + its own ROM symbol; LC II has Egret
if(ROM_NAME STREQUAL "Color-Classic.ROM")
    set(MAC_V8_CUDA 1)
else()
    set(MAC_V8_CUDA 0)
endif()

# Optional: disk image. DISK_NAME selects the file in ROM/ (any size up to
# the flash budget, ~2.89 MB: app partition 0x3F0000 minus code and ROM).
# custom_753.dsk = 2880K image from tools/build_disk.py.
# Falls back to the plain System 7.5.3 boot floppy if the custom disk has not
# been built (tools/build_disk.py).
set(DISK_NAME "custom_753.dsk")
if(NOT EXISTS "${ROM_DIR}/${DISK_NAME}" AND EXISTS "${ROM_DIR}/OS_750_boot.dsk")
    message(STATUS "${DISK_NAME} not found - falling back to OS_750_boot.dsk")
    set(DISK_NAME "OS_750_boot.dsk")
endif()
string(MAKE_C_IDENTIFIER "${DISK_NAME}" DISK_SYM)
set(HAS_DISK_IMAGE 0)
if(EXISTS "${ROM_DIR}/${DISK_NAME}")
    list(APPEND EMBED_FILES "${ROM_DIR}/${DISK_NAME}")
    set(HAS_DISK_IMAGE 1)
    file(SIZE "${ROM_DIR}/${DISK_NAME}" DISK_BYTES)
    message(STATUS "Embedding disk image: ${DISK_NAME} (${DISK_BYTES} bytes)")
else()
    message(STATUS "No disk image found (${DISK_NAME}) - will boot to '?' icon")
endif()

idf_component_register(
    SRCS "main.cpp"
         "mac_emulator.c"
         "mac_memory.c"
         "mac_video.c"
         "mac_via.c"
         "mac_egret.c"
         "mac_touch.c"
         "mac_scc.c"
         "mac_asc.c"
         "mac_scsi.c"
         "mac_iwm.c"
         "mac_v8video.c"
         "mac_v8disk.c"
         "mac_rtc.c"
         "mac_disk_overlay.c"
         "mac_kbd.c"
    INCLUDE_DIRS "."
    REQUIRES log driver esp_timer display musashi freertos esp_psram
    EMBED_FILES ${EMBED_FILES}
)

# Tell the C code whether disk image is available
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HAS_DISK_IMAGE=${HAS_DISK_IMAGE}
    MAC_ROM_LC_II=${MAC_ROM_LC_II}
    MAC_V8_CUDA=${MAC_V8_CUDA}
    DISK_SYM=${DISK_SYM}
)
