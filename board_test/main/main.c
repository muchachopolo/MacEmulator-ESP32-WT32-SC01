/*
 * Board Diagnostic Tool for WT32-SC01-3.4
 * Probes and reports all hardware capabilities over serial.
 */
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_psram.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <spi_flash_mmap.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_st7789.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "DIAG";

/* WT32-SC01-3.4 pin assignments */
#define LCD_PIN_MOSI    GPIO_NUM_13
#define LCD_PIN_CLK     GPIO_NUM_14
#define LCD_PIN_CS      GPIO_NUM_15
#define LCD_PIN_DC      GPIO_NUM_21
#define LCD_PIN_RST     GPIO_NUM_22
#define LCD_PIN_BL      GPIO_NUM_23
#define TOUCH_PIN_SDA   GPIO_NUM_18
#define TOUCH_PIN_SCL   GPIO_NUM_19
#define TOUCH_I2C_ADDR  0x38

static void print_separator(const char *title)
{
    printf("\n");
    printf("========================================\n");
    printf("  %s\n", title);
    printf("========================================\n");
}

static void probe_chip(void)
{
    print_separator("CHIP INFO");

    esp_chip_info_t info;
    esp_chip_info(&info);

    const char *model_name = "Unknown";
    switch (info.model) {
        case CHIP_ESP32:   model_name = "ESP32"; break;
        case CHIP_ESP32S2: model_name = "ESP32-S2"; break;
        case CHIP_ESP32S3: model_name = "ESP32-S3"; break;
        case CHIP_ESP32C3: model_name = "ESP32-C3"; break;
        default: break;
    }

    printf("  Model:          %s\n", model_name);
    printf("  Revision:       v%d.%d\n", info.revision / 100, info.revision % 100);
    printf("  Cores:          %d\n", info.cores);
    printf("  Features:       ");
    if (info.features & CHIP_FEATURE_WIFI_BGN) printf("WiFi ");
    if (info.features & CHIP_FEATURE_BT) printf("BT ");
    if (info.features & CHIP_FEATURE_BLE) printf("BLE ");
    if (info.features & CHIP_FEATURE_EMB_FLASH) printf("EmbFlash ");
    if (info.features & CHIP_FEATURE_EMB_PSRAM) printf("EmbPSRAM ");
    printf("\n");
    printf("  CPU freq:       %d MHz\n", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    printf("  MAC:            %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void probe_flash(void)
{
    print_separator("FLASH INFO");

    uint32_t flash_size;
    esp_err_t ret = esp_flash_get_size(NULL, &flash_size);
    if (ret == ESP_OK) {
        printf("  Flash size:     %lu MB (%lu bytes)\n",
               (unsigned long)(flash_size / (1024 * 1024)),
               (unsigned long)flash_size);
    } else {
        printf("  Flash size:     ERROR reading (%s)\n", esp_err_to_name(ret));
    }

    uint32_t chip_id;
    ret = esp_flash_read_id(NULL, &chip_id);
    if (ret == ESP_OK) {
        printf("  Flash chip ID:  0x%06lX\n", (unsigned long)chip_id);
        printf("  Manufacturer:   0x%02lX\n", (unsigned long)(chip_id & 0xFF));
        printf("  Device ID:      0x%04lX\n", (unsigned long)((chip_id >> 8) & 0xFFFF));
    }

    printf("  Flash speed:    %s\n",
#if CONFIG_ESPTOOLPY_FLASHFREQ_80M
        "80 MHz"
#elif CONFIG_ESPTOOLPY_FLASHFREQ_40M
        "40 MHz"
#else
        "unknown"
#endif
    );
    printf("  Flash mode:     %s\n",
#if CONFIG_ESPTOOLPY_FLASHMODE_DIO
        "DIO"
#elif CONFIG_ESPTOOLPY_FLASHMODE_QIO
        "QIO"
#else
        "unknown"
#endif
    );
}

static void probe_psram(void)
{
    print_separator("PSRAM INFO");

    size_t psram_size = esp_psram_get_size();
    if (psram_size > 0) {
        printf("  PSRAM detected: YES\n");
        printf("  PSRAM size:     %lu MB (%lu bytes)\n",
               (unsigned long)(psram_size / (1024 * 1024)),
               (unsigned long)psram_size);

        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        printf("  PSRAM free:     %lu bytes\n", (unsigned long)free_psram);
        printf("  PSRAM largest:  %lu bytes\n",
               (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

        /* Test PSRAM with a large allocation */
        size_t test_size = 1 * 1024 * 1024; /* 1MB */
        void *test = heap_caps_malloc(test_size, MALLOC_CAP_SPIRAM);
        if (test) {
            memset(test, 0xAA, test_size);
            uint8_t *p = (uint8_t *)test;
            bool ok = true;
            for (size_t i = 0; i < test_size; i += 4096) {
                if (p[i] != 0xAA) { ok = false; break; }
            }
            printf("  PSRAM 1MB test: %s\n", ok ? "PASS" : "FAIL");
            heap_caps_free(test);
        } else {
            printf("  PSRAM 1MB test: ALLOC FAILED\n");
        }

        /* Try progressively larger allocations to find actual usable size */
        for (int mb = 1; mb <= 8; mb++) {
            test_size = mb * 1024 * 1024;
            test = heap_caps_malloc(test_size, MALLOC_CAP_SPIRAM);
            if (test) {
                heap_caps_free(test);
                printf("  PSRAM %dMB alloc: OK\n", mb);
            } else {
                printf("  PSRAM %dMB alloc: FAIL (max usable < %dMB)\n", mb, mb);
                break;
            }
        }
    } else {
        printf("  PSRAM detected: NO\n");
        printf("  (No PSRAM found or init failed)\n");
    }
}

static void probe_memory(void)
{
    print_separator("MEMORY SUMMARY");

    printf("  Internal SRAM:\n");
    printf("    Total free:   %lu bytes\n",
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("    Largest blk:  %lu bytes\n",
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    printf("    DMA-capable:  %lu bytes\n",
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA));

    printf("  Total heap:\n");
    printf("    Free:         %lu bytes\n",
           (unsigned long)esp_get_free_heap_size());
    printf("    Min free ever:%lu bytes\n",
           (unsigned long)esp_get_minimum_free_heap_size());
}

static bool probe_display(void)
{
    print_separator("DISPLAY (ST7796 via SPI)");

    /* Initialize SPI bus */
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = LCD_PIN_MOSI;
    bus_cfg.miso_io_num = GPIO_NUM_NC;
    bus_cfg.sclk_io_num = LCD_PIN_CLK;
    bus_cfg.quadwp_io_num = GPIO_NUM_NC;
    bus_cfg.quadhd_io_num = GPIO_NUM_NC;
    bus_cfg.max_transfer_sz = 480 * 20 * 2;

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        printf("  SPI bus init:   FAIL (%s)\n", esp_err_to_name(ret));
        return false;
    }
    printf("  SPI bus init:   OK (SPI2_HOST)\n");

    /* Create panel IO */
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.dc_gpio_num = LCD_PIN_DC;
    io_cfg.cs_gpio_num = LCD_PIN_CS;
    io_cfg.pclk_hz = 40 * 1000 * 1000;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    io_cfg.spi_mode = 0;
    io_cfg.trans_queue_depth = 10;

    ret = esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io);
    if (ret != ESP_OK) {
        printf("  SPI panel IO:   FAIL (%s)\n", esp_err_to_name(ret));
        return false;
    }
    printf("  SPI panel IO:   OK (40MHz)\n");

    /* Create panel */
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = LCD_PIN_RST;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_cfg.bits_per_pixel = 16;

    ret = esp_lcd_new_panel_st7789(io, &panel_cfg, &panel);
    if (ret != ESP_OK) {
        printf("  LCD panel:      FAIL (%s)\n", esp_err_to_name(ret));
        return false;
    }
    printf("  LCD panel:      OK (ST7789/ST7796)\n");

    ret = esp_lcd_panel_reset(panel);
    printf("  Panel reset:    %s\n", ret == ESP_OK ? "OK" : esp_err_to_name(ret));

    ret = esp_lcd_panel_init(panel);
    printf("  Panel init:     %s\n", ret == ESP_OK ? "OK" : esp_err_to_name(ret));

    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, false, true);
    esp_lcd_panel_disp_on_off(panel, true);

    /* Turn on backlight */
    gpio_config_t bl_cfg = {};
    bl_cfg.pin_bit_mask = (1ULL << LCD_PIN_BL);
    bl_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&bl_cfg);
    gpio_set_level(LCD_PIN_BL, 1);
    printf("  Backlight:      ON\n");

    /* Draw a test pattern: fill screen with blue */
    uint16_t *buf = heap_caps_malloc(480 * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (buf) {
        /* Fill with blue (RGB565: 0x001F) */
        for (int i = 0; i < 480; i++) buf[i] = 0x001F;
        for (int y = 0; y < 320; y++) {
            /* Change color per region: blue/green/red/white */
            uint16_t color;
            if (y < 80) color = 0x001F;       /* Blue */
            else if (y < 160) color = 0x07E0;  /* Green */
            else if (y < 240) color = 0xF800;  /* Red */
            else color = 0xFFFF;               /* White */
            for (int i = 0; i < 480; i++) buf[i] = color;
            esp_lcd_panel_draw_bitmap(panel, 0, y, 480, y + 1, buf);
            vTaskDelay(1); /* Let DMA complete */
        }
        printf("  Test pattern:   DRAWN (blue/green/red/white bands)\n");
        heap_caps_free(buf);
    } else {
        printf("  Test pattern:   ALLOC FAIL\n");
    }

    return true;
}

static void probe_touch(void)
{
    print_separator("TOUCH (FT5x06 via I2C)");

    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.scl_io_num = TOUCH_PIN_SCL;
    bus_cfg.sda_io_num = TOUCH_PIN_SDA;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &bus);
    if (ret != ESP_OK) {
        printf("  I2C bus init:   FAIL (%s)\n", esp_err_to_name(ret));
        return;
    }
    printf("  I2C bus init:   OK (SCL=%d, SDA=%d)\n", TOUCH_PIN_SCL, TOUCH_PIN_SDA);

    /* Scan I2C bus for devices */
    printf("  I2C scan:       ");
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        ret = i2c_master_probe(bus, addr, 50);
        if (ret == ESP_OK) {
            printf("0x%02X ", addr);
            found++;
        }
    }
    if (found == 0) printf("(no devices found)");
    printf("\n");

    /* Try to read FT5x06 chip ID */
    i2c_master_dev_handle_t dev = NULL;
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = TOUCH_I2C_ADDR;
    dev_cfg.scl_speed_hz = 400000;

    ret = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (ret == ESP_OK) {
        /* Read FT5x06 firmware version (register 0xA6) and chip ID (0xA3) */
        uint8_t reg, val;

        reg = 0xA3;  /* Chip ID */
        ret = i2c_master_transmit_receive(dev, &reg, 1, &val, 1, 100);
        if (ret == ESP_OK) {
            printf("  FT5x06 chip ID: 0x%02X", val);
            if (val == 0x55) printf(" (FT5316)");
            else if (val == 0x06) printf(" (FT5x06)");
            else if (val == 0x36) printf(" (FT6336)");
            else if (val == 0x64) printf(" (FT6436)");
            printf("\n");
        } else {
            printf("  FT5x06 chip ID: READ FAIL (%s)\n", esp_err_to_name(ret));
        }

        reg = 0xA6;  /* Firmware version */
        ret = i2c_master_transmit_receive(dev, &reg, 1, &val, 1, 100);
        if (ret == ESP_OK) {
            printf("  FT5x06 FW ver: 0x%02X\n", val);
        }

        reg = 0xA8;  /* Panel ID */
        ret = i2c_master_transmit_receive(dev, &reg, 1, &val, 1, 100);
        if (ret == ESP_OK) {
            printf("  FT5x06 panel:  0x%02X\n", val);
        }

        /* Read a touch point to see if touch works */
        printf("  Touch test:     Checking for touch in 3 seconds...\n");
        printf("                  (Touch the screen now!)\n");
        for (int attempt = 0; attempt < 30; attempt++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            reg = 0x02;  /* Number of touch points */
            ret = i2c_master_transmit_receive(dev, &reg, 1, &val, 1, 10);
            if (ret == ESP_OK && (val & 0x0F) > 0 && (val & 0x0F) <= 5) {
                uint8_t data[5];
                reg = 0x03;
                ret = i2c_master_transmit_receive(dev, &reg, 1, data, 4, 10);
                if (ret == ESP_OK) {
                    int x = ((data[0] & 0x0F) << 8) | data[1];
                    int y = ((data[2] & 0x0F) << 8) | data[3];
                    printf("  Touch detected: x=%d, y=%d\n", x, y);
                }
                break;
            }
        }
    }
}

void app_main(void)
{
    printf("\n\n");
    printf("########################################\n");
    printf("#   WT32-SC01-3.4 Board Diagnostic     #\n");
    printf("########################################\n");

    probe_chip();
    probe_flash();
    probe_psram();
    probe_memory();
    probe_display();
    probe_touch();

    print_separator("DIAGNOSTIC COMPLETE");
    printf("  Board is ready for inspection.\n");
    printf("  Display should show color bands.\n");
    printf("  Check results above.\n");
    printf("========================================\n\n");

    /* Keep running so serial monitor stays open */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
