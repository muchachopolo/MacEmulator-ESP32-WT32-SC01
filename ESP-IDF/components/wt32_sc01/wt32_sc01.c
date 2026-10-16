#include "wt32_sc01.h"
#include "esp_log.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

static const char *TAG = "WT32_LCD";
esp_lcd_panel_handle_t lcd_panel;

void lcd_init() {
    ESP_LOGI(TAG, "Initializing LCD...");

    // Configure SPI bus
    spi_bus_config_t bus_config = {
        .sclk_io_num = WT32_LCD_CLK,
        .mosi_io_num = WT32_LCD_MOSI,
        .miso_io_num = TFT_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = WT32_LCD_WIDTH * WT32_LCD_HEIGHT * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(WT32_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));

    // Configure SPI device
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = WT32_LCD_DC,
        .cs_gpio_num = WT32_LCD_CS,
        .pclk_hz = 40 * 1000 * 1000, // 40MHz
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
    };
    esp_lcd_panel_io_handle_t io_handle;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(WT32_LCD_HOST, &io_config, &io_handle));

    // Configure LCD panel
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = WT32_LCD_RST,
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &lcd_panel));

    // Reset the LCD and apply initialization commands
    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_panel));

    // Set display orientation and turn on backlight
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(lcd_panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(lcd_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd_panel, true));

    gpio_set_direction(WT32_LCD_BACKLIGHT, GPIO_MODE_OUTPUT);
    gpio_set_level(WT32_LCD_BACKLIGHT, 1);

    ESP_LOGI(TAG, "LCD initialized.");
}

void fill_screen(uint16_t color) {
    uint16_t *color_buffer = heap_caps_malloc(WT32_LCD_WIDTH * WT32_LCD_HEIGHT * sizeof(uint16_t), MALLOC_CAP_DMA);
    assert(color_buffer != NULL);

    for (int i = 0; i < WT32_LCD_WIDTH * WT32_LCD_HEIGHT; i++) {
        color_buffer[i] = color;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(lcd_panel, 0, 0, WT32_LCD_WIDTH, WT32_LCD_HEIGHT, color_buffer));
    free(color_buffer);
}
