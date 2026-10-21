/*
 * Macintosh Classic Emulator for WT32-SC01-3.4
 *
 * Main entry point. Initializes display hardware, then starts
 * the Mac emulator on a dedicated high-priority task.
 */
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern "C" {
#include "display.h"
#include "mac_emulator.h"
#include "mac_video.h"
#include "mac_touch.h"
}

static const char *TAG = "main";

/* Emulator task stack size: generous for 68K emulation + call depth */
#define EMU_TASK_STACK_SIZE     (32 * 1024)
#define EMU_TASK_PRIORITY       (configMAX_PRIORITIES - 2)
#define EMU_TASK_CORE           1   /* Run CPU emulator on core 1 */

#define VIDEO_TASK_STACK_SIZE   (8 * 1024)
#define VIDEO_TASK_PRIORITY     (configMAX_PRIORITIES - 3)
#define VIDEO_TASK_CORE         0   /* Run video renderer on core 0 */

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== Macintosh Classic Emulator ===");
    ESP_LOGI(TAG, "Board: WT32-SC01-3.4 (ESP32-WROVER-B)");
    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "Free PSRAM: %lu bytes",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Step 1: Initialize display hardware */
    ESP_LOGI(TAG, "Initializing display...");
    if (!display_init()) {
        ESP_LOGE(TAG, "Display initialization failed!");
        return;
    }
    display_set_brightness(100);

    /* Step 2: Initialize emulator (loads ROM, allocates RAM) */
    ESP_LOGI(TAG, "Initializing emulator...");
    if (!mac_emu_init()) {
        ESP_LOGE(TAG, "Emulator initialization failed!");
        return;
    }

    ESP_LOGI(TAG, "Free heap after init: %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "Free PSRAM after init: %lu bytes",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Step 3: Start emulator on dedicated core */
    ESP_LOGI(TAG, "Starting emulation on core %d...", EMU_TASK_CORE);
    xTaskCreatePinnedToCore(
        mac_emu_run,            /* Task function */
        "mac_emu",              /* Name */
        EMU_TASK_STACK_SIZE,    /* Stack size */
        NULL,                   /* Parameter */
        EMU_TASK_PRIORITY,      /* Priority */
        NULL,                   /* Task handle */
        EMU_TASK_CORE           /* Core */
    );

    /* Step 4: Start video renderer on core 0 (separate from CPU emulation) */
    ESP_LOGI(TAG, "Starting video renderer on core %d...", VIDEO_TASK_CORE);
    xTaskCreatePinnedToCore(
        mac_video_task,         /* Task function */
        "mac_video",            /* Name */
        VIDEO_TASK_STACK_SIZE,  /* Stack size */
        NULL,                   /* Parameter */
        VIDEO_TASK_PRIORITY,    /* Priority */
        NULL,                   /* Task handle */
        VIDEO_TASK_CORE         /* Core */
    );

    /* Step 5: Touch sampler (core 0, ~60 Hz, independent of emulation
     * speed). No-op when TOUCH_SAMPLER_TASK=0: the emulation loop then
     * polls the panel inline. */
    touch_sampler_start();

    /* Main task can idle - emulator and video run independently */
    ESP_LOGI(TAG, "Emulator and video started. Main task idling.");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
