# o-platform —— FoloToy AI Passport 多合一固件

<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

**项目名：`o-platform`。** 面向 **FoloToy AI Passport** 工牌（ESP32-C3）的多合一固件。当前已落地
**PPT 遥控**（蓝牙 HID 键盘）与**语音输入**（端侧采集 → PC 流式语音识别 → 把文字打回桌面）两个功能，
让它们在**一套共享蓝牙栈上共存互不干扰**。

**愿景：** `o-platform` 的目标是一个**插件式平台**——让开发者能方便地把社区/他人的功能**移植**进来，
按本项目的分层规约归位、复用同一条 BLE/Wi‑Fi/内存预算，而不是各做一个互不兼容的分支固件。本文档的
「移植参考」与「内存/栈工程」两节，正是为降低移植门槛而写。

---

## 移植参考

`o-platform` 不是从零写，而是"底座 + 若干移植功能"的组合，遵循仓库移植管线
（[`docs/development/porting-pipeline.zh_CN.md`](docs/development/porting-pipeline.zh_CN.md)）。
donor 代码**按本平台分层重组、并非整仓搬运**，且文件头保留出处注释。

| 部分 | 上游 / donor | 许可 | 落地形态 |
|---|---|---|---|
| **底座平台** | [`rvaim/ai-passport`](https://github.com/rvaim/ai-passport)（插件化平台：`.pap` 包 / BLE 安装 / `passport_core`·`ui`·`runtime`） | — | `o-platform/` 基线 |
| **UI 风格参考** | [`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport)（官方固件，像素风语言） | — | 仅视觉借鉴 |
| **Wi‑Fi 配网** | 最终采用**小智的热点配网**：[`78/esp-wifi-connect`](https://github.com/78/esp-wifi-connect)（softAP + captive portal） | MIT | `components/passport_wifi_ap/`（早期 BLE 配网 `main/ble_prov.c` 已重构为热点配网） |
| **语音输入** | [`zhaohuaxiaoy/folo-ai-passport-voice`](https://github.com/zhaohuaxiaoy/folo-ai-passport-voice) | MIT | `components/passport_voice/` + PC 端 `companion/` |
| **PPT 遥控** | [`YeatsLiao/ai-passport-ppt`](https://github.com/YeatsLiao/ai-passport-ppt) | MIT | 经官方 `esp_hid` 组件，落在 `components/passport_ppt/` |

---

## 硬件与构建

- **ESP32-C3**（RISC-V 单核、约 400 KB SRAM、**无 PSRAM**）、8 MB flash；240×320 SPI + LVGL；
  UP/DOWN/OK 三枚 ADC 按键；ES8311 音频 + I2S；CW2017 电量计；USB-Serial/JTAG 刷写看日志。
- ESP-IDF **v5.5.3**。Windows 用 `tools/build.ps1`（E 盘 ccache + 文件锁重试）：
  ```powershell
  .\tools\build.ps1          # 构建
  .\tools\build.ps1 -Flash   # 构建并刷 COM6
  ```
  详见 [`docs/development/build-and-test.zh_CN.md`](docs/development/build-and-test.zh_CN.md)。

---

## 各功能怎么用

**PPT 遥控（HID 键盘）：** 进 **PPT 遥控** 页 → 系统蓝牙设置里配对成键盘 → 上键=上一页(←)、
下键=下一页(→)、**OK 短按**=开始放映(F5 + macOS 组合键)并启动计时、**下键长按**=退出(Esc)+重置计时。

**语音输入：** 进 **语音输入** 页 → 运行 PC 伴生程序（`companion/`）→ 按住说话，识别文字打进当前
聚焦窗口并在屏上回显。

**电子工牌：** 主页显示工牌字段与头像；经设备内传输页（`main/transfer_page.c`）改。

---

## 内存 / 栈工程 —— 怎么让语音和 PPT 共存

这是 `o-platform` 的核心。ESP32-C3 无 PSRAM，全部功能跑在几十 KB 堆里，而配网、**语音**、
**PPT/HID** 都要同时用蓝牙，Wi‑Fi 还要抢同一块内存。靠下面四件事把共存做出来：

### 1. 单一共享 NimBLE 栈
配网、语音、HID 共用**一条 NimBLE 栈**，生命周期由 `main/ble_prov.c` 集中独占。donor 组件不得
另起栈——两条栈会把 controller/host 占用翻倍、直接爆预算。（关掉 `ROLE_OBSERVER`/`HOST_BASED_PRIVACY`
省 RAM；保留 `ROLE_CENTRAL`，因为语音音频需要**外设主动**发起 ATT MTU 交换。）

### 2. 按页面做身份隔离 —— 让 Windows 把语音和键盘分开对待
Windows 判定"这是我的键盘"同时看**广播内容**（外观 + HID `0x1812` UUID）**和蓝牙地址**。所以两个
功能在每个维度都隔离：

| | PPT 页 | 语音页 |
|---|---|---|
| 名字 | `AI Passport` | `AI Passport Voice` |
| 广播 16-bit UUID | `0xA2B0` + `0x1812` | 仅 `0xA2B0` |
| 外观 appearance | `0x03C1`（键盘） | `0x0000`（普通外设） |
| 地址 | public | **派生的静态随机地址**（每次开机稳定） |

**随机地址是关键**：用不同地址广播，Windows（把 public 地址记成了键盘）就**认不出、不会来抢**语音
连接，桌面 companion 才能拿到干净的 GATT 连接。（顺带修了两个坑：`ble_gap_conn_active()` 其实返回的是
"本机是否作为 master 正在连接"的 0/1、不是连接数，改成自维护计数；`adv_restart` 现在先 `adv_stop`
再 `adv_start`，否则换身份/换地址不生效。）

### 3. 双连接槽 + 处理重复配对
`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2`（controller `BLE_MAX_ACT=6`）让主机自动重连的 HID 键盘和语音
桌面端各占一槽。`gap_event_cb` 处理 `BLE_GAP_EVENT_REPEAT_PAIRING`（删旧 bond、返回 `RETRY`），
避免对端删了配对记录再来重配对时被 NimBLE "silently ignoring" 丢弃。

### 4. Wi‑Fi 不常驻 + 零堆缓冲
- **把 Wi‑Fi 让给 BLE。** Wi‑Fi 只在开机为 SNTP 校时启动，完成/超时即释放（**约腾出 ~99 KB**）；
  `show_voice`/`show_ppt` 起 BLE 前也会先 `wifi_sta_stop()`。若 Wi‑Fi 常驻，`NimBLE + esp_hid + 语音
  GATT 服务` 会因堆不足起不来 → 广播失败、桌面端扫不到。
- **运行时零堆。** 语音事件下行队列（4×512B）、CTRL 暂存（2KB）、音频静态环形缓冲、以及
  `event_worker` **任务栈全部放 `.bss`**——因为 `nimble_port_init`（controller）已吃掉约 44.7 KB 堆，
  运行时再分配任务栈会饿死 `ble_hs_start`、引发反复重启。（worker 栈定 4096；实测 5120 会让 BLE 起不来。）

---

## 配套工具（电脑端）

`companion/`（移植自语音 donor）：`relay.py`（BLE↔ASR 中转，`语音中转-relay.bat` / `语音助手-GUI.bat`）、
`probe.py`、`asr_client.py`（火山流式 ASR）。

```bash
cd companion
python -m venv .venv && .venv/Scripts/pip install -r requirements.txt
# 把火山 ASR 的 API Key 写进 config.local.json（格式见 config.example.json）
```

> **密钥安全**：ASR key 只放 `companion/config.local.json`，该文件已被 **git 忽略**。绝不提交真实密钥。

## 状态

开发 / 演示分支。语音输入与 PPT 控制已在真机走通，非正式发布。
