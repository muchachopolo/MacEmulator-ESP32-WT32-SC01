idf_component_register(
    SRCS "lcd.cpp" "lvgl_port_v8.cpp"
    INCLUDE_DIRS "include"
    REQUIRES log driver espressif__esp32_display_panel
)

project (lcd)