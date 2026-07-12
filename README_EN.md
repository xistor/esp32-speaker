# ESP Speaker

## Introduction

A DIY Bluetooth speaker using ESP32.

## Software and Hardware Environment

- ESP32-WROVER N16R8 module, must support Classic Bluetooth, higher PSRAM is better
- Main peripherals: I2S DAC PCM5102, display ST7789, touch controller CST816S
- ESP-IDF development framework, use version 5.5.3 or later
- VSCODE
- C++

## Features

- Bluetooth A2DP sink (audio receiver)
- AVRCP media control and metadata retrieval (track / artist / playback status / cover art / play progress)
- Music spectrum display
- Touch screen interaction
- Codec support: SBC, AAC
  - AAC support requires ESP-IDF v6.2.0 or above and the following options enabled:  
    - CONFIG_BT_A2DP_USE_EXTERNAL_CODEC=y  
    - CONFIG_BT_A2DP_CODEC_AAC_ENABLED=y

## Reference

https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/api-reference/bluetooth/esp_a2dp.html
