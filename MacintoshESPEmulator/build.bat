@echo off
REM ESP-IDF v6.1 build script for MacintoshESPEmulator
REM Sets up environment from EIM installation

set IDF_PATH=C:\esp\v6.1\esp-idf
set IDF_TOOLS_PATH=C:\Espressif\tools
set IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python\v6.1\venv
set ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011
set OPENOCD_SCRIPTS=C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\share\openocd\scripts

REM Clear MSYS/MinGW environment to prevent ESP-IDF from rejecting the shell
set ESP_IDF_VERSION=6.1.0
set MSYSTEM=
set MSYS=
set MINGW_PREFIX=
set MINGW_CHOST=

set PATH=C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\esp-clang\esp-20.1.1_20250829\esp-clang\bin;C:\Espressif\tools\esp-rom-elfs\20241011\;C:\Espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin;C:\Espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\esp32ulp-elf\bin;C:\Espressif\tools\idf-exe\1.0.3\;C:\Espressif\tools\ninja\1.12.1\;C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\bin;C:\Espressif\tools\xtensa-esp-elf-gdb\16.3_20250913\xtensa-esp-elf-gdb\bin;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\xtensa-esp-elf\bin;C:\Espressif\tools\python\v6.1\venv\Scripts;C:\Program Files\Git\cmd;%PATH%

cd /d %~dp0
idf.py %*
