idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES log esp_hw_support esp_system heap spi_flash esp_psram esp_lcd esp_driver_spi esp_driver_gpio esp_driver_i2c freertos
)
