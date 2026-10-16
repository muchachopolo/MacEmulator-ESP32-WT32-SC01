idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES log driver esp_system esp_lcd esp_system freertos wt32_sc01)
