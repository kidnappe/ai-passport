# AI Passport —— 语音输入 + PPT 遥控 演示分支

<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

面向 **FoloToy AI Passport**（ESP32-C3 工牌）的固件演示分支。在底座平台之上，同时提供两个
可并存工作的蓝牙功能：

- **PPT 遥控** —— 工牌作为蓝牙 HID **键盘**广播（官方 `esp_hid` / NimBLE 后端），在
  Windows / macOS / Linux 上控制幻灯片：上一页 / 下一页、开始放映（F5 及 macOS 组合键）、
  退出放映（Esc），并在屏上显示演讲计时。
- **语音输入** —— 自定义 BLE GATT 服务（`0xA2B0`：CTRL / EVENT / AUDIO）把端侧采集的音频
  （IMA ADPCM）推给 PC 伴生程序做语音识别，并把转写文本回显到工牌。

两个功能靠**按页面的广播身份隔离**避免互相干扰：PPT 页以 `AI Passport` 广播，携带 HID UUID +
键盘外观（并用 public 地址）；语音页在一个派生的随机地址上以 `AI Passport Voice` 广播、不带 HID
UUID，这样主机不会把语音连接误认成键盘而抢占。

## 硬件 / 构建

- 目标：**ESP32-C3**，8 MB flash，**无 PSRAM**，ESP-IDF **v5.5.3**，LVGL 驱动 240×320 SPI 屏。
- 构建与刷写：见 [`docs/development/build-and-test.zh_CN.md`](docs/development/build-and-test.zh_CN.md)
  （Windows 上 `tools/build.ps1` 封装了 ESP-IDF 5.5.3 环境并带重试）。
- 贡献者与 AI Agent 的仓库规范：[`AGENTS.zh_CN.md`](AGENTS.zh_CN.md)。

## 本分支要点

- BLE 生命周期由 `main/ble_prov.c` 集中管理（配网 / 语音 / HID 共用同一 NimBLE 栈），
  双连接槽让主机自动重连的键盘与语音伴生端可同时保持连接。
- `components/passport_ppt` —— 用官方 `esp_hid` 组件实现 HID 键盘（手写 NimBLE GATT HID 在
  Windows 上不被识别为键盘，见移植说明）。
- `components/passport_voice` —— 音频采集、ADPCM 推流与 `0xA2B0` GATT 服务。
- 电子工牌（姓名 / 学校等字段）支持 WiFi 传输，以及 BLE / 热点配网。

## 状态

演示 / 开发分支。语音输入与 PPT 控制已在真机走通，非正式发布。语音输入的 PC 伴生程序在另一个项目里。
