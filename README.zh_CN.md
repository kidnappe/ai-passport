# AI Passport —— 语音输入 + PPT 遥控

<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

面向 **FoloToy AI Passport** 工牌（ESP32-C3）的固件演示分支。在底座平台之上，同时提供两个
共用一条 BLE 栈、可并存工作的功能：

- **PPT 遥控** —— 工牌作为蓝牙 HID **键盘**出现。
- **语音输入** —— 端侧采集的音频经自定义 BLE 服务推给 PC 伴生程序做流式语音识别，再把文字
  回打到你正在用的桌面窗口里。

---

## 基座与移植来源（这个项目是从哪来的）

本工程**不是从零写**，而是"底座 + 若干移植功能"的组合。依据仓库移植管线
（[`docs/development/porting-pipeline.zh_CN.md`](docs/development/porting-pipeline.zh_CN.md)）：

| 部分 | 上游 / donor | 许可 | 落地形态 |
|---|---|---|---|
| **底座平台** | [`rvaim/ai-passport`](https://github.com/rvaim/ai-passport)（插件化平台：`.pap` 包 / BLE 安装 / `passport_core`·`ui`·`runtime`） | — | `o-platform/` 基线 |
| **UI 风格参考** | [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport)（官方固件，像素风语言） | — | 仅视觉借鉴，非代码移植 |
| **Wi‑Fi 配网** | [`killhello/ai-pass-port-wifi`](https://github.com/killhello/ai-pass-port-wifi) | — | 自研重写：`main/ble_prov.c` + `components/passport_wifi_ap/` |
| **语音输入** | [`zhaohuaxiaoy/folo-ai-passport-voice`](https://github.com/zhaohuaxiaoy/folo-ai-passport-voice) | MIT | 按本平台分层重组为 `components/passport_voice/` + PC 端 `companion/` |
| **PPT 遥控** | [`YeatsLiao/ai-passport-ppt`](https://github.com/YeatsLiao/ai-passport-ppt) | MIT | 经官方 `esp_hid` 组件实现，落在 `components/passport_ppt/` |

移植文件头部保留出处注释（如 `components/passport_voice/src/voice_ble.c`、
`components/passport_ppt/src/passport_ppt.c`）。

---

## 硬件

- **ESP32-C3**（RISC-V 单核、约 400 KB SRAM、**无 PSRAM**）、8 MB flash。
- 240×320 SPI 屏（LVGL）、UP/DOWN/OK 三枚 ADC 按键、ES8311 音频 codec + I2S、
  CW2017 电量计、USB-Serial/JTAG（刷写与看日志）。

## 构建与刷写

ESP-IDF **v5.5.3**，见 [`docs/development/build-and-test.zh_CN.md`](docs/development/build-and-test.zh_CN.md)。
Windows 上 `tools/build.ps1` 已封装工具链（E 盘 ccache + Defender 文件锁重试）：

```powershell
.\tools\build.ps1          # 增量构建
.\tools\build.ps1 -Flash   # 构建后刷写 COM6
```

---

## 各功能怎么用

### 1. 电子工牌
主页显示工牌字段（昵称 / 学校 / 专业 / 学号）与头像。进设备内传输页起一个 HTTP 服务，
用配套传输工具上传修改；回主页即刷新，无需重启。

### 2. PPT 遥控（蓝牙 HID 键盘）
1. 进 **PPT 遥控** 页（开始以 `AI Passport`、键盘外观广播）。
2. 在系统蓝牙设置里配对 → 变成普通键盘（Windows / macOS / Linux）。
3. 按键：
   - **上键** —— 上一页（←）
   - **下键** —— 下一页（→）
   - **OK 短按** —— 开始放映（F5，并附 macOS 组合键）+ 启动演讲计时
   - **下键长按** —— 退出放映（Esc）+ 重置计时

### 3. 语音输入
1. 进 **语音输入** 页（在独立随机地址上以 `AI Passport Voice` 广播，避免被桌面键盘链路抢占）。
2. 运行 PC 伴生程序（见下），按住说话，识别文字会被打进当前聚焦窗口并回显到屏幕。

---

## 配套工具（电脑端）

语音配套程序在 **[`companion/`](companion/)**（移植自 `zhaohuaxiaoy/folo-ai-passport-voice`）：

- `relay.py` —— BLE↔ASR 中转，`语音中转-relay.bat` / `语音助手-GUI.bat` 启动它；
- `probe.py` —— 快速扫描/连接自检；
- `asr_client.py` —— 流式 ASR 客户端（火山引擎）。

初始化：
```bash
cd companion
python -m venv .venv && .venv/Scripts/pip install -r requirements.txt
# 把火山 ASR 的 API Key 写进 config.local.json（格式见 config.example.json）
```

> **密钥安全**：ASR 的 API Key 只放 `companion/config.local.json`，该文件已被 **git 忽略**。
> 绝不提交真实密钥，`config.example.json` 只是占位模板。

---

## 为多功能共存做的内存 / 栈工程

ESP32-C3 无 PSRAM，整套功能跑在几十 KB 堆里。为了让配网 + 语音 + HID 同时可用：

- **BLE 与 Wi‑Fi 抢同一块堆 → Wi‑Fi 不常驻**。开机时 Wi‑Fi 只为 SNTP 校时启动，校时完成
  或超时即释放（约腾出 ~99 KB）；`show_voice`/`show_ppt` 起 NimBLE 栈前也会先 `wifi_sta_stop()`。
- **单一 NimBLE 栈共享**。配网、语音、HID 都跑在同一条栈上，生命周期由 `main/ble_prov.c`
  集中独占（移植进来的代码不得再起第二条栈）。
- **零堆静态缓冲**。语音事件下行队列（4×512B）、CTRL 暂存（2KB）、音频静态环形缓冲、
  以及 `event_worker` 任务栈全部是 **静态（.bss）**——因为 `nimble_port_init`（controller）
  已吃掉约 44.7 KB 堆，这些再走运行时分配就会饿死 BLE 启动、引发反复重启。
- **双连接槽**。`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2`（controller `BLE_MAX_ACT=6`），让主机
  自动重连的 HID 键盘与语音桌面端各占一槽、互不抢占。

## 状态

开发 / 演示分支。语音输入与 PPT 控制已在真机走通，非正式发布。
