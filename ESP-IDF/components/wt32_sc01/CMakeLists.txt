idf_component_register(SRCS "wt32_sc01.c"
                       INCLUDE_DIRS "include"
                       REQUIRES log driver esp_lcd esp_system freertos)

project (wt32_sc01)