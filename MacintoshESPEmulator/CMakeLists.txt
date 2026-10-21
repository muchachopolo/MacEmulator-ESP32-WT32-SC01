cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "components/musashi"
    "components/display"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# IDF v6.1 spinlock.h does ++ on a volatile under SPIRAM DUPLDST; gnu++26 rejects it
idf_build_set_property(CXX_COMPILE_OPTIONS "-Wno-error=volatile" APPEND)
project(MacintoshEmulator)
