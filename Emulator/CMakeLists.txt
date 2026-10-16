# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(EXTRA_COMPONENT_DIRS 
    components/lcd
)

add_compile_definitions(ESP_PANEL_KCONFIG_IGNORE=1)
add_compile_definitions(ESP_PANEL_CONF_PATH=${CMAKE_SOURCE_DIR}/ESP_Panel_Conf.h)
add_compile_definitions(ESP_PANEL_SUPPORTED_BOARD_PATH=${CMAKE_SOURCE_DIR}/ESP_Panel_Board_Custom.h)

project(68KEmulator)