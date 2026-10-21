/*
 * Display driver for WT32-SC01-3.4 (ST7796 + FT5x06)
 * Uses native ESP-IDF esp_lcd + esp_lcd_touch APIs. No managed components.
 *
 * Hardware:
 *   LCD: ST7796 480x320 SPI, MOSI=13, CLK=14, CS=15, DC=21, RST=22, BL=23
 *   Touch: FT5x06 I2C, SDA=18, SCL=19
 */
#include <string.h>
#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_st7789.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "display.h"

static const char *TAG = "display";

/* WT32-SC01-3.4 pin assignments */
#define LCD_SPI_HOST    SPI2_HOST
#define LCD_PIN_MOSI    GPIO_NUM_13
#define LCD_PIN_CLK     GPIO_NUM_14
#define LCD_PIN_CS      GPIO_NUM_15
#define LCD_PIN_DC      GPIO_NUM_21
#define LCD_PIN_RST     GPIO_NUM_22
#define LCD_PIN_BL      GPIO_NUM_23
#define LCD_SPI_FREQ_HZ (40 * 1000 * 1000)

#define TOUCH_PIN_SDA   GPIO_NUM_18
#define TOUCH_PIN_SCL   GPIO_NUM_19
#define TOUCH_I2C_ADDR  0x38     /* FT5x06 default address */

static esp_lcd_panel_handle_t lcd_panel = NULL;
static SemaphoreHandle_t draw_done_sem = NULL;
static i2c_master_bus_handle_t i2c_bus = NULL;
static i2c_master_dev_handle_t touch_dev = NULL;

/* DMA transfer complete callback */
static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t need_yield = pdFALSE;
    xSemaphoreGiveFromISR(draw_done_sem, &need_yield);
    return (need_yield == pdTRUE);
}

static bool init_lcd(void)
{
    draw_done_sem = xSemaphoreCreateBinary();
    if (!draw_done_sem) return false;

    /* Initialize SPI bus */
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = LCD_PIN_MOSI;
    bus_cfg.miso_io_num = GPIO_NUM_NC;
    bus_cfg.sclk_io_num = LCD_PIN_CLK;
    bus_cfg.quadwp_io_num = GPIO_NUM_NC;
    bus_cfg.quadhd_io_num = GPIO_NUM_NC;
    bus_cfg.max_transfer_sz = DISPLAY_WIDTH * 20 * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    /* Create LCD panel IO (SPI) */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.dc_gpio_num = LCD_PIN_DC;
    io_cfg.cs_gpio_num = LCD_PIN_CS;
    io_cfg.pclk_hz = LCD_SPI_FREQ_HZ;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    io_cfg.spi_mode = 0;
    io_cfg.trans_queue_depth = 10;
    io_cfg.on_color_trans_done = on_color_trans_done;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_cfg, &io_handle));

    /* Create ST7789/ST7796 panel (ST7796 is register-compatible) */
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = LCD_PIN_RST;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_cfg.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &lcd_panel));

    /* Initialize and configure the panel */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(lcd_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(lcd_panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd_panel, true));

    return true;
}

static bool init_touch(void)
{
    /* Initialize I2C master bus */
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.scl_io_num = TOUCH_PIN_SCL;
    bus_cfg.sda_io_num = TOUCH_PIN_SDA;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed (touch disabled): %s", esp_err_to_name(ret));
        return false;
    }

    /* Add FT5x06 touch device */
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = TOUCH_I2C_ADDR;
    dev_cfg.scl_speed_hz = 400000;
    ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &touch_dev);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Touch device add failed: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "Touch initialized (FT5x06 @ 0x%02X)", TOUCH_I2C_ADDR);
    return true;
}

static void init_backlight(void)
{
    gpio_config_t bl_cfg = {};
    bl_cfg.pin_bit_mask = (1ULL << LCD_PIN_BL);
    bl_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&bl_cfg);
    gpio_set_level(LCD_PIN_BL, 1);  /* Backlight on */
}

bool display_init(void)
{
    ESP_LOGI(TAG, "Initializing display (native esp_lcd)...");

    if (!init_lcd()) {
        ESP_LOGE(TAG, "LCD init failed");
        return false;
    }

    init_touch();       /* Non-fatal if touch fails */
    init_backlight();

    ESP_LOGI(TAG, "Display initialized: %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    return true;
}

void display_draw(int x, int y, int w, int h, const uint16_t *data)
{
    esp_err_t err = esp_lcd_panel_draw_bitmap(lcd_panel, x, y, x + w, y + h, data);
    if (err != ESP_OK) {
        static bool once = false;
        if (!once) {
            once = true;
            ESP_LOGE(TAG, "draw_bitmap failed @%d,%d %dx%d: %s", x, y, w, h, esp_err_to_name(err));
        }
        return;
    }
    /* Wait for DMA transfer to complete */
    xSemaphoreTake(draw_done_sem, portMAX_DELAY);
}

bool display_get_touch(int *x, int *y)
{
    if (!touch_dev) return false;

    /* Read FT5x06 touch data registers (0x00-0x06) */
    uint8_t reg = 0x00;
    uint8_t data[7];
    esp_err_t ret = i2c_master_transmit_receive(touch_dev, &reg, 1, data, sizeof(data), 10);
    if (ret != ESP_OK) return false;

    /* Number of touch points is in register 0x02 */
    uint8_t num_points = data[2] & 0x0F;
    if (num_points == 0 || num_points > 5) return false;

    /* First touch point: X = reg[3] + reg[4], Y = reg[5] + reg[6] */
    int raw_x = ((data[3] & 0x0F) << 8) | data[4];
    int raw_y = ((data[5] & 0x0F) << 8) | data[6];

    /* The touch coordinates may need swapping for landscape mode */
    *x = raw_x;
    *y = raw_y;
    return true;
}

void display_set_brightness(int percent)
{
    /* Simple on/off for GPIO backlight */
    gpio_set_level(LCD_PIN_BL, percent > 0 ? 1 : 0);
}
