# Musashi 68000 CPU Emulator component for ESP-IDF
#
# m68kops.c and m68kops.h must be pre-generated using m68kmake before building.
# To generate:
#   cd components/musashi
#   gcc -o m68kmake m68kmake.c
#   ./m68kmake . m68k_in.c

# Register the component first (required for ESP-IDF build system)
idf_component_register(
    SRCS "m68kcpu.c" "m68kops.c" "softfloat/softfloat.c"
    INCLUDE_DIRS "."
    REQUIRES log
)

# Verify generated files exist
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/m68kops.c" OR NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/m68kops.h")
    message(FATAL_ERROR
        "Musashi: m68kops.c and/or m68kops.h not found in ${CMAKE_CURRENT_LIST_DIR}.\n"
        "These files must be generated before building:\n"
        "  cd components/musashi\n"
        "  gcc -o m68kmake m68kmake.c\n"
        "  ./m68kmake . m68k_in.c\n"
    )
endif()

# Suppress warnings in third-party Musashi code
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wno-sign-compare
    -Wno-maybe-uninitialized
    -Wno-implicit-fallthrough
    -Wno-parentheses
    -Wno-shift-count-overflow
    -Wno-overflow
    -Wno-type-limits
    -Wno-format-overflow
    -Wno-implicit-function-declaration
    -Wno-array-bounds
)
