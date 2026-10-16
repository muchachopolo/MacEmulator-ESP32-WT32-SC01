# Embed ROM and disk images from the ROM directory
set(ROM_DIR "${CMAKE_SOURCE_DIR}/../ROM")

# Required: Classic.ROM
set(EMBED_FILES "${ROM_DIR}/Classic.ROM")

# Optional: disk image
set(HAS_DISK_IMAGE 0)
if(EXISTS "${ROM_DIR}/OS_750_boot.dsk")
    list(APPEND EMBED_FILES "${ROM_DIR}/OS_750_boot.dsk")
    set(HAS_DISK_IMAGE 1)
    message(STATUS "Embedding disk image: OS_750_boot.dsk (1.44MB)")
else()
    message(STATUS "No disk image found (OS_750_boot.dsk) - will boot to '?' icon")
endif()

idf_component_register(
    SRCS "main.cpp"
         "mac_emulator.c"
         "mac_memory.c"
         "mac_video.c"
         "mac_via.c"
         "mac_scc.c"
         "mac_iwm.c"
    INCLUDE_DIRS "."
    REQUIRES log driver esp_timer display musashi freertos esp_psram
    EMBED_FILES ${EMBED_FILES}
)

# Tell the C code whether disk image is available
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HAS_DISK_IMAGE=${HAS_DISK_IMAGE}
)
