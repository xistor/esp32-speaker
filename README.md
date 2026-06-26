# X Speaker 2.0

[ENGLISH README](README_EN.md)

## 简介
使用ESP32 DIY的蓝牙音箱

## 软硬件环境

- ESP32-WROVER N16R8模组， 须支持classic bt的, PSRAM越大越好
- 主要外设：I2S解码 PCM5102， 屏幕 ST7789, 触摸 CST816S。
- ESP-IDF 开发框架，使用5.5.3之后的版本。
- VSCODE
- C++

## 功能

- Bluetooth A2DP sink（蓝牙音频接收）
- AVRCP 媒体控制与元数据获取（曲目/艺人/播放状态/封面图/播放进度）
- 音乐频谱显示
- 触摸屏交互

## 参考

https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/api-reference/bluetooth/esp_a2dp.html