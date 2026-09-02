# 方案：以 BLE 时代为主进度，配网改为热点配网（保留开机自启 WiFi）

> 提出时间：2026-09-01
> 状态：**待用户确认**
> 目标硬件：ESP32-C3 工牌（8 MB flash，无 PSRAM），240×320 SPI 屏

---

## 一、目标与约束（用户已确认）

| 项 | 决定 |
|---|---|
| 最终固件定位 | **纯 BLE 语音，无小智**（回到 8-30 BLE 时代功能） |
| 配网方式 | BLE 配网 → **热点配网**（softAP + HTTP captive portal） |
| 保留 | **开机自启 WiFi**（自动连已保存网络 + 掉线重连） |
| 保留 | BLE 语音通道（GATT 0xA2B0 语音对话） |
| 热点组件 | **复用当前已验证的 `components/passport_wifi_ap`** |

**不能引入小智语音**：`passport_xiaozhi`、`xiaozhi_audio`、`s_auto_start_xiaozhi` 均不进主进度。

---

## 二、操作步骤

### 第 1 步：备份当前小智移植进度（含文档 + 项目概览）

当前 `o-platform/` 是小智移植完成态。在还原为 BLE 主进度之前，先整体留档：

- 新建备份目录：`E:\code\ai passport\backup\2026-09-01_xiaozhi移植完成态\`
- 备份内容：
  - `o-platform/` 完整源码（**排除** `build/`、`managed_components/`、`sdkconfig`、`backup/`，与既有备份习惯一致）
  - 相关文档：`docs/porting-pipeline.zh_CN.md`、`docs/xiaozhi-handoff-2026-08-31.zh_CN.md` 及其余小智相关 `.md`
  - **新增** `README.md`：整份小智移植项目概览（见第四节大纲）
- 输出：`o-platform-src.tar.gz` + `README.md`

### 第 2 步：还原 BLE 时代备份为主进度

- 将 `o-platform/backup/backup-20260830-201450/source/o-platform/` 的内容**覆盖回** `E:\code\ai passport\o-platform\`
  - 恢复 `main.c`（BLE 配网逻辑）、`main/ble_prov.*`、`main/wifi_sta.*`、`main/transfer_page.*`
  - 恢复 `components/`：只留 `bsp / human_display / passport_core / passport_ui / passport_voice`
  - **删除**（主进度不再需要）：`components/passport_xiaozhi`、`components/xiaozhi_audio`、`components/passport_wifi_ap`、`main/wifi_ap_prov` 相关
  - 分区表、sdkconfig.defaults、partitions.csv 恢复为 BLE 时代版本

### 第 3 步：把热点配网并入 BLE 主进度

**3.1 拷入组件**：把当前小智代码里的 `components/passport_wifi_ap/`（已验证）整体拷入主进度 `components/`。

**3.2 改 `main/CMakeLists.txt`**：
- `SRCS`：`ble_prov.c` → 保留（BLE 语音通道仍用），`wifi_sta.c`、`transfer_page.c`、`main.c` 不变
- 新增 `PRIV_REQUIRES`：`passport_wifi_ap`
- `REQUIRES` 补 `esp_wifi`（若 hotspot 组件需）——组件自身已 REQUIRES esp_wifi，main 一般无需重复

**3.3 改 `main/main.c` 的 `wifi_prov_task`（唯一配网替换点）**：

```c
// 改前
esp_err_t err = ble_prov_start();
...
if (ble_prov_get_creds(ssid, sizeof(ssid), pass, sizeof(pass))) break;
...
ble_prov_stop();

// 改后
esp_err_t err = wifi_ap_prov_start();
...
if (wifi_ap_prov_get_creds(ssid, sizeof(ssid), pass, sizeof(pass))) break;
...
wifi_ap_prov_stop();
```

**接口完全对等**（start / get_creds / stop），只改函数名与头文件 include，配网页状态文案可复用。

**3.4 严格不动的 BLE 语音调用点**（这些是语音通道，不是配网，**必须保留**）：
- `show_voice()` 内 `ble_prov_start()`（进语音页开通道，行 ~1077）
- 网络设置页"蓝牙开关" `ble_prov_start()`（行 ~1201）
- 开机初始化 `if (s_bt_enabled) ble_prov_start()`（行 ~1421）
- 由 `ble_prov.h` 提供的 `ble_prov_is_running()` 等语音相关调用

> ⚠️ 关键认知：BLE 时代 `ble_prov` 同时承担"配网 + 语音通道"（main.c 行 ~1195 注释）。**只替换配网那一段**，语音通道调用一律保留，否则会砍掉 BLE 语音。

**3.5 保留开机自启 WiFi + 重连**：`wifi_sta.c` 原逻辑不动（`wifi_sta_init` + `wifi_sta_set_auto_connect(true)` + 掉线重连），主进度原样保留。

### 第 4 步：构建验证 + 真机走查

- PowerShell + 系统默认 TEMP（勿设 `E:\tmp`）
- `IDF_TOOLS_PATH=E:\esp\.espressif`、`IDF_PYTHON_ENV_PATH=E:\esp\.espressif\python_env\idf5.5_py3.12_env`
- `idf.py build` → `idf.py -p COM6 flash`
- 走查：开机自动连 WiFi → 断网重连；进配网页出现 softAP 热点 → 手机连热点 + 网页提交 → 配网成功连上 WiFi；进语音页 BLE 0xA2B0 通道可用。

---

## 三、风险与规避

| 风险 | 规避 |
|---|---|
| 误删 BLE 语音通道 | 只替换 `wifi_prov_task` 内配网函数；语音/蓝牙开关/开机处的 `ble_prov_start` 一字不动 |
| `passport_wifi_ap` 依赖 `esp_http_server`/`json`/`nvs_flash` 未在主进度启用 | 组件 CMakeLists 自带 REQUIRES，主进度只需在 main 加 `passport_wifi_ap` |
| 热点组件是 C++（`.cc`） | ESP-IDF 原生支持 C++，CMake 自动编译，无需额外配置 |
| NVS 命名冲突 | hotspot 用 `wifi` 命名空间，wifi_sta 用 `wifi_sta`，互不冲突 |
| 还原后 build 目录/sdkconfig 残留 | 还原时删 build、删 sdkconfig，重新 `idf.py set-target esp32c3` |
| BLE 时代无 PSRAM，热点 + BLE 栈并发内存 | 沿用既有 BLE guard/堆优化；热点配网需停 BLE 语音时按既有逻辑处理（参考小智版 BLE guard） |

---

## 四、小智移植项目概览 README 大纲（第 1 步新增）

```markdown
# 小智移植项目概览（备份快照 2026-09-01）
## 一、目标与硬件
## 二、三条移植线（底座 / 热点配网 / 小智语音）
## 三、目录结构与关键组件
## 四、构建与刷机环境
## 五、移植管线（docs/porting-pipeline.zh_CN.md 六阶段）
## 六、当前进度快照（本次备份的 commit/内容）
## 七、与 BLE 时代的关系（本次改配网背景）
```

---

## 五、待确认点

1. 第 3.3 步替换后，配网页的 UI 文案是否沿用 BLE 时代（"热点已开启/等待手机配置"等）？默认沿用。
2. 还原时是否保留当前小智代码在 `projects/` 下的笔记？默认保留备份，主进度 `projects/xiaozhi-voice` 可删。
3. 确认后即按此方案执行。
