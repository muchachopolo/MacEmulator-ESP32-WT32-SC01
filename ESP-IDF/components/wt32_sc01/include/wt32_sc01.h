#ifndef WT32_SC01_H
#define WT32_SC01_H

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"

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
void lcd_init(void);
void fill_screen(uint16_t color);

#endif // WT32_SC01_H
