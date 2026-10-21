# ESP Speaker

[ENGLISH README](README_EN.md)

## 简介
使用ESP32 DIY的蓝牙音箱

## 软硬件环境

- ESP32-WROVER N16R8模组， 须支持classic bt的, PSRAM越大越好
- 主要外设：I2S DAC PCM5102， 屏幕 ST7789, 触摸 CST816S。
- ESP-IDF 开发框架，使用5.5.3之后的版本。
- VSCODE
- C++

## 功能

- Bluetooth A2DP sink（蓝牙音频接收）
- AVRCP 媒体控制、绝对音量控制与元数据获取（曲目/艺人/播放状态/封面图/播放进度）
- 音量自动保存：蓝牙（AVRCP 0..127）与 AirPlay（dB）分别持久化到 NVS，重启后保持上次音量
- AirPlay 1（RAOP）接收
  - ALAC 44.1kHz / 16bit / 立体声
  - 音量、曲目/艺人/进度元数据
  - 封面图可通过设置页 “Album Art” 开关即时切换显示，无需重启或重新连接 AirPlay；关闭后只丢弃收到的封面不做解码，蓝牙也不再请求封面。若希望连封面传输一起避免（部分发送端会在每次歌词/元数据更新时重发大图，导致音量响应变慢），需要在 menuconfig 中关闭 `CONFIG_AIRPLAY_ENABLE_ARTWORK`（默认关闭）
  - 打开 `CONFIG_AIRPLAY_ENABLE_ARTWORK` 后 mDNS 固定声明 `md=0,1,2`；固件升级或首次连接后，发送端需重新连接一次 AirPlay 才会读到新的能力声明
  - 触摸屏播放/暂停/上一首/下一首通过 DACP 控制 AirPlay 发送端（需要发送端支持 DACP 遥控）
- AirPlay 与蓝牙在设置页二选一，切换后会自动重启以生效
- 音乐频谱显示（蓝牙与 AirPlay 模式均可用）
- 触摸屏交互（状态栏显示蓝牙 / AirPlay 连接状态、电池电量）
- 编码支持SBC，AAC
    AAC 支持需要ESP-IDF v6.2.0以上，且打开以下两个宏  
    CONFIG_BT_A2DP_USE_EXTERNAL_CODEC=y  
    CONFIG_BT_A2DP_CODEC_AAC_ENABLED=y

## 参考

https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/api-reference/bluetooth/esp_a2dp.html