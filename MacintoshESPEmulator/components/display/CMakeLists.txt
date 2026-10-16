idf_component_register(
    SRCS "display.cpp"
    INCLUDE_DIRS "include"
    REQUIRES log esp_lcd esp_driver_spi esp_driver_gpio esp_driver_i2c freertos
)
