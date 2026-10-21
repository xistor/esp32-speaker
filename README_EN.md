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
- AVRCP media control, absolute volume, and metadata retrieval (track / artist / playback status / cover art / play progress)
- Volume is saved: Bluetooth (AVRCP 0..127) and AirPlay (dB) are persisted separately in NVS, so the last level survives a reboot
- AirPlay 1 (RAOP) receiver
  - ALAC 44.1 kHz / 16-bit / stereo
  - Volume, track/artist/progress metadata
  - Cover art can be toggled instantly from the “Album Art” switch in Settings — no reboot and no AirPlay reconnect needed. When off, incoming artwork is discarded without decoding, and Bluetooth stops requesting it. To avoid the artwork transfer as well (some senders resend a large JPEG on every metadata/lyric update, which delays volume changes), disable `CONFIG_AIRPLAY_ENABLE_ARTWORK` in menuconfig (off by default)
  - With `CONFIG_AIRPLAY_ENABLE_ARTWORK` enabled the receiver always advertises `md=0,1,2`; after a firmware update the sender has to connect to AirPlay once to read the new capability
  - On-device play/pause/previous/next buttons control the AirPlay sender via DACP (the sender must support DACP remote control)
- AirPlay and Bluetooth are selected in Settings; the device restarts automatically after switching
- Music spectrum display (available in both Bluetooth and AirPlay modes)
- Touch screen interaction (status bar shows Bluetooth / AirPlay connection state and battery level)
- Codec support: SBC, AAC
  - AAC support requires ESP-IDF v6.2.0 or above and the following options enabled:  
    - CONFIG_BT_A2DP_USE_EXTERNAL_CODEC=y  
    - CONFIG_BT_A2DP_CODEC_AAC_ENABLED=y

## Reference

https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/api-reference/bluetooth/esp_a2dp.html
