/* Embeds the ROM and disk images with the same symbol names that ESP-IDF's
 * EMBED_FILES produces (_binary_<name>_start/_end). Files are found via
 * the assembler include path (-Wa,-I<ROM dir>). */
#ifndef ROM_FILE
#define ROM_FILE "Color-Classic.ROM"
#endif
#ifndef ROM_SYM
#define ROM_SYM Color_Classic_ROM
#endif
#define CAT3(a, b, c) a##b##c
#define START(s) CAT3(_binary_, s, _start)
#define END(s)   CAT3(_binary_, s, _end)

    .section .rodata
    .balign 16
    .global START(ROM_SYM), END(ROM_SYM)
START(ROM_SYM):
    .incbin ROM_FILE
END(ROM_SYM):

#if HAS_DISK_IMAGE
    .balign 16
    .global _binary_OS_750_boot_dsk_start, _binary_OS_750_boot_dsk_end
_binary_OS_750_boot_dsk_start:
    .incbin "OS_750_boot.dsk"
_binary_OS_750_boot_dsk_end:
#endif

    .section .note.GNU-stack,"",@progbits
