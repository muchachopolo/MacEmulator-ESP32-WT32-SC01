#include <stdio.h>
#include "LCD.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "MAIN";

extern "C" void app_main()
{
    ESP_LOGI(TAG, "Starting WT32-SC01 setup...");
    lcd_init();

    while (1)
    {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}