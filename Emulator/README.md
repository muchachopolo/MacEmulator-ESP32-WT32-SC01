# Macintosh Emulator

## Currently Supported Devices 

### Only WT32-SC01 for now

| Devices   | WT32-SC01  | WT32-SC01+ | ESP32S3SPI35 | ESP32S335D
|:---------  |:-----------:|:----------:|:--------:|:-----------:
|Manufacturer|Wireless-Tag|Wireless-Tag|Makerfabs|Makerfabs
|MCU   |ESP32|ESP32-S3   |ESP32-S3    | ESP32-S3
|Display Size | 3.5" |3.5"  |3.5"  |3.5" 
|Resolution|480x320|480x320|480x320|480x320
|Display Controller|ST7796|ST7796UI|ILI9488|ILI9488
|Display Interface|SPI|8Bit Parallel|SPI|16Bit Parallel
|Touch Controller|FT5x06|FT5x06|FT5x06|FT5x06
|Flash Size |4MB|8MB|16MB|16MB
|PSRAM Size |4MB|2MB|2MB|2MB
|Wi-Fi|Yes|Yes|Yes|Yes
|Bluetooth|BT/BLE 4.x|BLE 5.x|BLE 5.x|BLE 5.x
|SD CARD |No|Yes|Yes|Yes
|OTG |No|Yes|Yes|Yes
|Datasheet|[PDF](datasheet/WT32-SC01_ESP32.pdf)|[PDF](datasheet/WT32-SC01_Plus_ESP32-S3.pdf)|[PDF](datasheet/Makerfabs-ESP32S3SPI35.PDF)|[PDF](datasheet/Makerfabs-ESP32S335D.pdf)
|Purchase Link|[BUY](https://bit.ly/wt32-sc01)|[BUY](https://bit.ly/wt32-sc01-plus)|[BUY](https://bit.ly/ESP32S3SPI35)|[BUY](https://bit.ly/ESP32S335D)
> Flash & PSRAM specifications are as per what I received.

## Getting Started
_Make sure you have installed ESP-IDF 5.0 (released version) and working from the command-line._