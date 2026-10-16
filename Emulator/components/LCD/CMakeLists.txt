idf_component_register(SRCS "LCD.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES log driver
                    )

project (LCD)