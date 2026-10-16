idf_component_register(
                        SRCS "LCD.cpp"
                        INCLUDE_DIRS "include"
                        REQUIRES log driver espressif__esp32_display_panel
                    )

project (LCD)