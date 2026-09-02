# AI Passport：键盘(HID) + 麦克风(语音) 一体 BLE 设备 —— 方案与进度

> 更新日期：2026-09-02　|　状态：**配对失败 + 语音 coexist 两大根因均已定位并修复（真机走查通过）**
> 关联：语音/PPT 广播身份隔离方案一已落地（见当日工作日志），本文聚焦其后的**配对失败根因**与**架构升级方向**。
> **结论先行**：
> 1. **配对转圈失败** = `gap_event_cb` 未处理 `BLE_GAP_EVENT_REPEAT_PAIRING`（NVS 残留旧 bond → Windows 删配对重连时被 host `silently ignoring pair request from bonded peer` 丢弃）。已修。
> 2. **键盘连上后语音 relay 报 `0xA2B2 not found`** = `voice_ble_register()` 的 `static bool s_registered` 守卫：`nimble_port_deinit` 每次拆掉整张 GATT 表，切页/开关蓝牙后重建时 esp_hid/配网都重注册、语音却因守卫被跳过 → 语音服务不在新表。已改为按 NimBLE 实例每次重注册；并对语音页启用**地址级身份隔离**（独立随机地址广播，Windows 不再抢连接管）。已修+用户确认功能正常。
> 候选 A/B/C/D 全部排除（见 §2.2）。

---

## 1. 用户目标与背景

设备需要**同时**被识别为两类 BLE 设备：

- **键盘操控**：PPT 遥控（HID 服务 0x1812，appearance=键盘 0x03C1）
- **麦克风/语音**：语音输入（自定义服务 0xA2B0，CTRL/EVENT/AUDIO 特征）

### 现状（已完成）
「广播身份隔离」方案一已落地并验证：
- 语音页广播 `AI Passport Voice`（只带 0xA2B0、普通外设外观）
- PPT 页广播 `AI Passport`（带 0xA2B0+0x1812、键盘外观 0x03C1）
- 语音桌面端（bleak）能扫到 `AI Passport Voice` 并推流

### 当前卡点
切到 **PPT 页**后，Windows 蓝牙设置添加设备配对 **转圈失败**（场景 A：全新配对失败）。
之前 PPT 能正常用，属**回归**。已用串口日志实锤：

```
I (23179) ble_prov: 设备已连接 (handle 1)
I (23180) passport_ppt: HID 主机已连接
```

**连接成功（handle=1）但无任何 SMP 配对/加密事件**（无 ENC_CHANGE、无 bond、无 security 日志）。

---

## 2. 配对失败根因排查

### 2.1 已确认事实
- GATT 连接建立成功，但 Windows 配对状态机卡在 SMP 协商前/中。
- 当前配对配置（`voice_ble.c`）：
  ```c
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;   // Just Works
  ble_hs_cfg.sm_bonding = 1;                          // 持久配对
  ```
  sdkconfig：`CONFIG_BT_NIMBLE_SM_SC=y`、`SECURITY_ENABLE=y`、`SM_LEGACY=y`。
- HID 服务（esp_hid 的 `nimble_hidd.c`）特征带 `READ_ENC|WRITE_ENC`，Windows 访问会触发配对。
- 已排除：**单线程手动跑 ar/ranlib 成功** → 非僵尸进程锁文件、非 C 盘满（见 §4 编译环境）。

### 2.2 候选根因与排查结论
| # | 候选根因 | 排查结论 |
|---|---------|---------|
| A | Windows 对 HOGP 键盘要求 MITM/Passkey，Just Works+SC 握手卡住 | ❌ 排除。DEBUG 日志显示配对请求根本没进入协商阶段 |
| B | `ble_store` 回调未初始化 → bond 存不下 | ❌ 排除。恰好相反：旧 bond **存在**（NVS_PERSIST 生效），才触发静默忽略 |
| C | `sm_sc` + `sm_legacy` 同时开导致 SC 实现问题 | ❌ 排除。SMP 未到密钥协商 |
| D | `MAX_CONNECTIONS` 连接槽耗尽 → SMP 资源不足 | ❌ 非根因。连接建立正常(handle=1)、MTU 协商成功；=2 已另行落实（双客户端并存所需，与配对无关） |
| **实** | **`gap_event_cb` 未处理 `BLE_GAP_EVENT_REPEAT_PAIRING`** | ✅ **真根因。见 §2.4** |

### 2.3 DEBUG 日志定位（已完成）
临时把日志三闸门全开（缺一不可）后重编刷机，抓一次 Windows 配对握手：
- `CONFIG_BT_NIMBLE_LOG_LEVEL_DEBUG=y`（NimBLE 内部 `BLE_HS_LOG_LVL`，仅这一项**不够**）
- `CONFIG_LOG_MAXIMUM_LEVEL=4`（否则 esp_log 在**编译期**就把 `ESP_LOGD` 裁掉，D 行根本不进二进制）
- 运行期 `esp_log_level_set("NimBLE", ESP_LOG_DEBUG)`（默认运行时级别是 INFO，会过滤掉 DEBUG）

抓到的决定性一行：
```
D NimBLE: looking up peer sec / looking up our sec
D NimBLE: silently ignoring pair request from bonded peer
I link_voice: 已连接 (handle 1)
I passport_ppt: HID 主机已连接
```
**一锤定音**：Windows 确实发来了 SMP 配对请求，但本机识别到"对端已 bond"（旧记录还在 NVS），
NimBLE host 在 `ble_sm_chk_repeat_pairing()` 里向应用抛 `BLE_GAP_EVENT_REPEAT_PAIRING` 事件；
应用未处理（回调返回 0 ≠ `BLE_GAP_REPEAT_PAIRING_RETRY`）→ host 落回"silently ignoring"分支，
丢弃配对请求。Windows 等不到响应 → 30s 超时"转圈失败"。

### 2.4 修复（已落地）
在 `main/ble_prov.c` 的 `gap_event_cb` 增加处理分支（对齐 NimBLE bleprph 官方示例）：
```c
case BLE_GAP_EVENT_REPEAT_PAIRING: {
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
        ble_store_util_delete_peer(&desc.peer_id_addr);   // 删旧 bond
    }
    return BLE_GAP_REPEAT_PAIRING_RETRY;                  // 让配对继续
}
```
语义正确性：对端能走到"重发配对请求"，说明它已丢失密钥；此时本机旧 bond 已无意义，
删除并按新配对重建 bond 是正解。验证（§2.3 的同一 DEBUG 版本）：Windows「删除设备→重新添加」后，
设置页「输入」分类出现 `AI Passport`、**类别=键盘、状态=已连接**，配对转圈不再失败。

> 「回归」的真相：与广播身份隔离方案一**无关**（配对请求在进入 SMP 前就被丢弃，与广播内容无关）。
> 时间线是——PPT 移植版曾在 Windows 上配对成功 → bond 持久化进 NVS → 后来 Windows 侧删除了配对
> 记录 → 本机旧 bond 成了死结，此后任何"全新配对"都会命中该分支失败。

---

## 3. 架构方向讨论：单 HID 服务 + 语音走自定义 Report（TI VoHoGP）

### 3.1 行业标准做法
世界上确实有「麦克风 + 键盘」一体的 BLE 设备，行业标准是 **TI 的
"Voice over HID over GATT (VoHoGP)"**（CC2640 带麦遥控器/耳机）。
核心：**只保留一个 HID 服务，语音作为 HID 的一个自定义 Report ID**（如 Report ID 11），
与键盘报告共用同一 HID 服务。TI 原话：

> "The advantage in using the adopted HID over GATT Profile (HoGP) is that operating systems
> generally already support this profile natively; thus eliminating the need to develop a custom GATT profile."

这样 Windows/Android/iOS 只认到**一个设备、一个 HID 服务**，配对/加密/重连走 HOGP 标准路径，
彻底绕开"多服务、多角色"的兼容性地狱。

### 3.2 现有架构 vs VoHoGP
| | 现架构 | VoHoGP |
|---|-------|--------|
| 服务数 | 3 个（HID + 语音 0xA2B0 + 配网 128bit） | 1 个 HID + 1 个配网 |
| 语音通道 | 独立 GATT 服务，变长分片 | HID 自定义 Report |
| 配对 | Windows 对"整个设备"做严格安全判断，多服务易卡 | 走 HOGP 标准，最稳 |

### 3.3 esp_hid 支持多 Report ID（关键突破口）
`nimble_hidd.c` 的 `create_hid_db()` 会解析 Report Map，把每个 Report ID 注册成**独立 Report 特征**。
只要在 Report Map 里再加一个自定义语音 Report，esp_hid 就自动在**同一 HID 服务**下注册并存，和键盘 Report 1 共存。

### 3.4 ⚠️ 致命约束（为何需谨慎）
语音通道真实数据模型**与固定长度 HID Report 冲突**：
- 音频帧：804 字节 IMA ADPCM，按 MTU(20~182) 动态分片，带 2 字节帧头 `[块序号][片序号|末片]` 做丢片重组，依赖 mbuf 池背压。
- CTRL/EVENT：变长 JSON（CTRL 下行 ≤2048B，EVENT 上行 ≤512B）。
- HID Report 是 **Report Map 里预先声明固定长度**（`Report Size × Report Count`），对端只按声明长度解析。

**强行纯 VoHoGP** 需废弃变长分片协议、重写为固定 20 字节 Report 流，丢片重组/mbuf 背压全部推倒重来——
与 TI 遥控器（固定小包语音帧）场景本质不同。**在配对根因未确认前，冒然大改架构是赌博。**

### 3.5 方向结论
DEBUG 日志已定位根因为**缺 `REPEAT_PAIRING` 处理**（见 §2.4），修复即解决 Windows 配对失败，
**无需**为此推翻现有双服务架构。三大架构方向（纯 VoHoGP / 修 SMP / 混合）**暂不启动**，
保留本 §3 作为将来若需进一步收敛服务数/配对模型时的备选参考。当前维持方案一（双服务 + 广播身份隔离）。

---

## 4. 编译环境问题（本次排障的副产物）

### 4.1 症状
`idf.py build` 反复失败，错误每次不同：
- `ar: unable to copy file 'libesp_xxx.a'; reason: Permission denied`（esp_adc / esp_http_server / http_parser 随机）
- `ccache ... deleting depfile: No error`

### 4.2 排查结论
- **手动单线程 ar/ranlib 完全成功** → 非僵尸进程锁文件、非 C 盘满、非权限问题。
- **根因：ninja 高并发编译时，多个 ar/ranlib 进程与 Windows Defender 实时扫描冲突**（偶发）。
- 已加 Defender 排除：`E:\code\ai passport\o-platform\build`。
- 已确认存在一个杀不掉的僵尸 gcc 进程（PID 27960，卡 9h+，无命令行）——但实测未锁关键文件。

### 4.3 缓解措施
- 已用 `-- -j 2` 低并发编译。
- 已设 `IDF_CCACHE_ENABLE=0` 禁 ccache（因 ccache 缓存在 C 盘，C 盘 96% 满诱发 `deleting depfile` 错误）。

> **2026-09-02 补充（本问题已解除）**：仓库固化的 `tools/build.ps1` 走
> `IDF_TOOLS_PATH=E:\esp\.espressif` + `CCACHE_DIR=E:\esp\ccache`（缓存搬离 C 盘）+ 内置 4 次
> 文件锁重试 + Defender 排除，多次全量/增量构建（含本轮 DEBUG/最终固件共 5 次）均**一次通过**，
> 未再复现 ar/ranlib Permission denied。无需再手动 `IDF_CCACHE_ENABLE=0` 或低并发。
> 另：在 Git Bash 里跑构建/刷机须绕开——ESP-IDF 的 `export.ps1` 检测到 MSYS 环境会拒绝，
> 且 `env -u MSYSTEM` 会让子进程 stdout 重定向失效；正确做法是写一个批处理里
> `set MSYSTEM=` 清空后经 `cmd /c` 调用 `tools/build.ps1`。

> ⚠️ 记忆红线：不要设 `$env:TEMP/E:\tmp`（曾致 gcc/ranlib 写临时 .s/.a 时 Permission denied）。

---

## 5. 当前进度清单

### ✅ 已完成
- [x] 广播身份隔离方案一：固件 + GUI 落地并刷机验证
  - `ble_prov.c`：双广播身份、`ble_prov_set_identity()`
  - `main.c`：show_voice/show_ppt 按页设身份
  - `companion/probe.py` + `relay.py`：`DEVICE_NAME = "AI Passport Voice"`
- [x] bleak 验证语音页广播 `AI Passport Voice`（无 HID 0x1812）
- [x] 串口实锤 PPT 配对失败：连接成功但无 ENC_CHANGE
- [x] 调研并锁定架构方向：TI VoHoGP（单 HID + 语音自定义 Report）——**后因根因明确而暂缓**
- [x] 排查 `ar Permission denied`：确认是 ninja 并发 + Defender 扫描冲突
- [x] 给 build 目录加 Defender 排除
- [x] DEBUG 三闸门（NimBLE DEBUG + LOG_MAXIMUM_LEVEL=4 + 运行期 tag 提级）刷入并抓到 `silently ignoring pair request from bonded peer`
- [x] **定位根因 = 缺 `BLE_GAP_EVENT_REPEAT_PAIRING` 处理**（候选 A/B/C/D 全排除）
- [x] **修复 `gap_event_cb`**：删旧 bond + 返回 RETRY；DEBUG 版本实证 Windows 配出「AI Passport=键盘 已连接」
- [x] `sdkconfig` 落实 `MAX_CONNECTIONS=2`（双客户端并存；注意 defaults 不覆盖已存在 sdkconfig 的陷阱，直接改 sdkconfig）
- [x] 出**最终固件**：移除测试钩子 + 还原全部临时 DEBUG 配置（保留 REPEAT_PAIRING 修复 + MAX2）；启动干净停主页
- [x] **coexist 根因确诊**：bleak dump 语音连接 GATT 全表 → 0xA2B0 缺失 → 定位为 `voice_ble_register` 的 `s_registered` 守卫在 NimBLE 拆表后跳过重注册
- [x] **修复 `voice_ble_register`**：拆分"一次性资源"与"每 NimBLE 实例必做的 GATT+listener 重注册"
- [x] **顺带修** `adv_restart`：先 `ble_gap_adv_stop` 再 start（解 EALREADY/地址不切换）；`ble_gap_conn_active()` 误用改为自维护 `s_conn_count`
- [x] **语音页地址级身份隔离**：派生静态随机地址广播，Windows 不再抢连接管
- [x] 用户走 PPT→主页→语音 拆表序列 → **功能全部正常**；串口见重注册成功 + 音频 notify 持续推流

### ⏳ 待办（残余，多为回归防护）
- [ ] 反复切页/开关蓝牙、Windows 删配对→重配对 长时间压力走查，确认无偶发 GATT/连接异常
- [ ] 若后续要收敛为单设备/单服务模型，再评估 §3 VoHoGP

---

## 6. 关键文件索引
| 文件 | 作用 |
|------|------|
| `main/ble_prov.c` | 广播身份隔离、adv_restart、CONNECT 事件处理 |
| `components/passport_voice/src/voice_ble.c` | 语音服务 0xA2B0、SMP 配置（sm_io_cap/bonding） |
| `components/passport_ppt/src/passport_ppt.c` | esp_hid 注册 HID 键盘服务 |
| `components/passport_voice/include/voice_ble.h` | 语音协议头（变长分片） |
| IDF `components/esp_hid/src/nimble_hidd.c` | HID 服务后端（多 Report ID 支持点） |
| IDF `components/bt/host/nimble/.../ble_svc_hid.c` | HID 服务特征（带 READ_ENC/WRITE_ENC） |
