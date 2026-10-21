/* Embeds the ROM and disk images with the symbol names ESP-IDF's EMBED_FILES
 * produces (_binary_<name>_start/_end), like host/rom_blobs.S. Bring-up only:
 * R5 loads them from the SD card instead. The built kernel7.img contains the
 * ROM and disk: never publish it. */
#define CAT3(a, b, c) a##b##c
#define START(s) CAT3(_binary_, s, _start)
#define END(s)   CAT3(_binary_, s, _end)

    .section .rodata
    .balign 16
    .global START(ROM_SYM), END(ROM_SYM)
START(ROM_SYM):
    .incbin ROM_FILE
END(ROM_SYM):

    .balign 16
    .global START(DISK_SYM), END(DISK_SYM)
START(DISK_SYM):
    .incbin DISK_FILE
END(DISK_SYM):
