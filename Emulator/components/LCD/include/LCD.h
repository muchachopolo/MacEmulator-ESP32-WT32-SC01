#include "driver/spi_master.h"

#ifndef WT32_SC01_H
    #define WT32_SC01_H

    // SPI LCD Pins
    #define WT32_LCD_HOST          HSPI_HOST
    #define TFT_MISO               -1
    #define WT32_LCD_MOSI          13
    #define WT32_LCD_CLK           14
    #define WT32_LCD_CS            15
    #define WT32_LCD_DC            21
    #define WT32_LCD_RST           22
    #define WT32_LCD_BACKLIGHT     23

    // LCD Dimensions
    #define WT32_LCD_WIDTH         320
    #define WT32_LCD_HEIGHT        480

    // Function declarations
#endif // WT32_SC01_H