#include "wt32_sc01.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"

#define TAG "MAIN"

void app_main() {
    ESP_LOGI(TAG, "Starting WT32-SC01 setup...");

    gpio_set_direction(WT32_LCD_RST, GPIO_MODE_OUTPUT);
    gpio_set_direction(WT32_LCD_CS, GPIO_MODE_OUTPUT);
    gpio_set_direction(WT32_LCD_DC, GPIO_MODE_OUTPUT);
    gpio_set_direction(WT32_LCD_BACKLIGHT, GPIO_MODE_OUTPUT);
    gpio_set_level(WT32_LCD_BACKLIGHT, 1);

    // Initialize LCD
    lcd_init();

    // Fill the screen with a color
    fill_screen(0xF800); // Red color
}
