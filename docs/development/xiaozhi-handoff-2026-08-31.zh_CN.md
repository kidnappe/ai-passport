# 小智语音链路 · 进度交接（2026-08-31 深夜 / 09-01 收尾）

> 范围：ESP32-C3 工牌（o-platform）上小智对话"能进/能开始/能打断，但设备没声音"的排查与修复。
> **08-31 代码改动已落地；09-01 完成常驻模型收尾（opus 建栈顺序、RTC 竞争、defer-audio、解除自启）；再经一次实锤【放弃自启】，编译+刷 COM6 验证通过。**
> 剩余：端到端语音（进小智页按 OK 说话）需真机物理验证。

---

## 0.6 本轮（09-01 第二次决策）放弃自启 · 解放占用（✅ 已落地并编译通过）

> 用户拍板：**放弃开机自启小智，解放被 WiFi 常驻占用的内存**；"减小 opus 任务栈"作为可选项。
> 根因确认：自启模式 WiFi 常驻 ~99KB，进小智页/音频 init 时堆只剩 ~65KB、最大连续块 49KB，
> 装不下 opus 栈(24KB)+编码器(17KB)+解码器(~10KB)=51KB → 解码器 `ret -7` → 下行无声。

**已定决策（用户确认 2026-09-01）：**
1. **放弃自启**：`main.c` `s_auto_start_xiaozhi = false`；`xiaozhi_auto_task` 不再创建；
   校时任务 `timesync_stop` 走"非自启"分支，`wifi_sta_stop()` 释放 ~99KB 堆；
   小智改为**用户手动进 应用→小智 页面**触发（`show_xiaozhi` 内 `destroy_home` + `defer=false`，堆完整再 init 音频）。
2. **opus 栈**：曾保持 24576（对齐原版，防 8192 爆栈教训）；**后经真机实测推翻，见 §0.7**——
   实际峰值已用仅 4880B，已安全减到 16384。

**本轮 09-01（二次）全部改动：**

| # | 文件 | 改动 |
|---|---|---|
| 1 | `main/main.c` | `s_auto_start_xiaozhi = true → false`（放弃自启）；`app_main` 不再创建 `xiaozhi_auto_task`；校时任务走"断开 WiFi 释放堆"分支；相关注释更新 |
| 2 | `components/passport_xiaozhi/src/passport_xiaozhi.c` | 清理上一轮遗留的诊断 `diag_listen_cb` 定时器残留（完整删除）；`s_defer_audio`/`ensure_audio_started` 注释改为"已放弃自启、此机制作防御" |
| 3 | `components/passport_xiaozhi/include/passport_xiaozhi.h` | `passport_xiaozhi_set_defer_audio` 注释更新（现默认 false，仅进页置 false） |
| 4 | `components/xiaozhi_audio/src/xiaozhi_audio.c` | **增强 opus 栈 HWM 采样**：新增 `s_first_enc_hwm_logged`，首次真实编码成功后在 `opus_task` 内立即打印 `首次真实编码: opus 栈 HWM(已用)=N/24576 字节`。旧 `send_task` HWM 只在队列排空 +10s 周期打印，连续开麦打不出；且文档警示"编码路径未跑过时 HWM 偏低无效"，故在 `celt_encode` 深调用真正执行后立刻采样，作减栈依据 |

**对比自启 vs 放弃自启（进小智页音频 init 时）：**

| 项 | 自启模式（失败） | 放弃自启（本次） |
|---|---|---|
| 开机 WiFi | 常驻 ~99KB | 校时后断开释放 ~99KB |
| 进小智页堆基线 | ~65KB，最大块 49KB | destroy_home 后 ~100KB+，最大块 ~90KB |
| 音频 init(24+17+10=51KB) | 装不进 49KB → 解码器 ret-7 | 装得进 ~90KB → 编解码器都成功 |
| WS TLS | 与音频抢内存 | 音频先占、WS 用剩余 |

> ⚠️ **§0.6 的"放弃自启后进页堆 ~100KB+ 完全够"是错误结论**，已由 §0.7 实测推翻。

---

## 0.7 本轮（09-01 第三次决策）四项确定性内存优化（✅ 编译验证，待刷机）

> **用户批评"你之前没想到内存优化？"属实。** 用户再实测抓日志（09:00）彻底推翻 §0.6 的乐观判断：
> 进小智页堆实测仅 **69KB**（不是假设的 136KB）——`destroy_home()` 释放的主页被 WiFi 重连 + WS
> 重新吃掉了。同时日志暴露三处**确定性内存浪费**：opus 栈 24576 实际只用 4880、`xz_send` 3KB 栈爆栈、
> 解码器 init 就常驻占 ~10KB。

**实测日志（用户进小智页 + 开麦，09:00）：**
```
音频 init 前: heap free=68912 最大连续块=53248   ← 进小智页后只有 69KB
开编码器前:  heap free=40472 最大连续块=29696
解码器: ret -7                                    ← 53KB 仍装不下 51KB
首次真实编码: opus 栈 HWM(已用)=4880/24576 字节   ← opus 栈真实只用 4.8KB！
enc malloc 丢包: malloc_fail 1→256                ← 堆枯竭
xz_send Stack overflow → 崩溃重启                 ← WS 发送 3KB 栈爆栈
```

**四项优化（本轮落地）：**

| # | 文件 | 改动 | 收益 |
|---|---|---|---|
| 1 | `xiaozhi_audio.c` | opus 栈 24576→**16384**（HWM 4880，3.4 倍余量） | 省 **8KB** 连续块 |
| 2 | `xiaozhi_audio.c` | `xz_send` 栈 3072→**6144**（修 WS 发送爆栈崩溃） | 不再崩溃 |
| 3 | `xiaozhi_audio.c` | 解码器**延迟到首个下行包**创建（去掉 init 里 `set_decode_rate(24000,60)` 预开；output_task 首包时自动创建） | init 时省 **~10KB** |
| 4 | `sdkconfig`+`sdkconfig.defaults` | `ESP_WIFI_DYNAMIC_RX_MGMT_BUFFER=y`（RX mgmt 缓冲动态化）+ `ESP_WIFI_ENTERPRISE_SUPPORT=n`（关企业 WiFi，用不到） | 省 ~4-6KB |

**优化后内存账**：init 时音频需求从 **51KB → 33KB**（16K opus 栈 + 17K 编码器，解码器延迟），
进小智页 69KB 堆下余量从 -18KB 变 +36KB，WS/WiFi 都有空间。解码器首包时才分配，彼时 init 峰值已过、
WiFi 常驻已稳定，~10KB 有充足余量。

**对比 IRAM_OPT 的取舍**：原版 `IRAM_OPT=n`/`RX_IRAM_OPT=n`，我们保持 `=y` **不改**——我们的目标是
**省 DRAM/连续堆**，IRAM_OPT=y 把 WiFi 代码放 IRAM 反而释放 DRAM，对我们有利；改 n 会多占 DRAM。

---

## 0.5 本轮（09-01）彻底重写 · 常驻模型（✅ 已收尾并真机验证）

> 用户拍板：**全组件彻底重写小智，目标"稳——一次跑通不再反复"**。
> 关键认知：原版 folotoy 稳，是因为编解码器在【开机、堆 164KB 完整时】一次打开、全程常驻、永不切换；
> 我们此前四轮无声，是因为自创「半双工按需切换」在碎片堆里反复 open/close 抢 17KB 连续块（`ret:-7`）。

**已定决策（用户确认）：**
1. 编解码器**常驻**：进小智页时（主页已释放）打开，全程不切换；
2. **彻底删除**半双工切换逻辑（`close_encoder()`/`close_decoder()` 已删除）；
3. **解除小智不自启限制**，允许开机自启连接服务器。

**本轮 09-01 全部改动（相对上次交接新增，均已落地并编译通过）：**

| # | 文件 | 改动 |
|---|---|---|
| 1 | `components/xiaozhi_audio/src/xiaozhi_audio.c` | **opus 大栈建栈顺序回归修复**（核心）：`init()` 改为 `s_inited=true` → **先建 opus 任务(24KB 栈)** → 再开编码器/解码器 → 最后建 input/output/send 小栈。编解码器若先分配会切碎堆、导致 24KB 连续块拿不到（真机实测 `opus=-1`） |
| 2 | `components/xiaozhi_audio/src/xiaozhi_audio.c` | 删除 `close_encoder()`/`close_decoder()`（上次交接标记的"待删除"项，本轮完成） |
| 3 | `main/main.c` | **RTC 竞争 bug 修复**：`xiaozhi_timesync_then_stop_wifi_task` 等待条件由 `now > 1000000000` 改为 `esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED`（RTC 电池域旧时间会让旧判断开机即成立、提前断 WiFi） |
| 4 | `main/main.c` | 新增 `static bool s_auto_start_xiaozhi = true;`，**重新启用** `xiaozhi_auto_task`（原被注释）；自启模式 `timesync` 任务不调 `wifi_sta_stop()`，保持 WiFi 常驻供小智自动激活 |
| 5 | `main/main.c` | `xiaozhi_auto_task` 调 `passport_xiaozhi_set_defer_audio(true)` 再 `passport_xiaozhi_start()`；`show_xiaozhi()` 在 `destroy_home()` 后置 `defer=false` |
| 6 | `components/passport_xiaozhi/src/passport_xiaozhi.c` + `include/passport_xiaozhi.h` | **defer-audio 机制**：新增 `s_defer_audio` 标志与 `passport_xiaozhi_set_defer_audio()`；`ensure_audio_started()` 在 defer=true 时跳过 `xiaozhi_audio_init()`（自启开机堆 ~70KB 不足以同时开音频 61KB + WS TLS 5KB，故先只连 WS，进小智页堆完整后再开音频） |

**下一步（全部完成）：**
1. ✅ 删除 `close_encoder()` / `close_decoder()` 函数体；
2. ✅ 检查 `s_out_cvt` 重采样器常驻语义（仅参数变化才关旧开新，符合常驻）；
3. ✅ 对齐 `passport_xiaozhi.c` 启动时序（`ensure_audio_started()` 在 WS 启动前调用，音频先行；自启时 defer）；
4. ✅ 修复开机校时时序竞争 bug（改等 SNTP 完成）；
5. ✅ 编译 → 刷 COM6 → 抓日志自证（三次刷机，第三次修复 defer 后开机干净）。

**真机验证结论（09-01 多次复位抓日志，均稳定）：**
- 开机自动连 WiFi（`CMCC-YFMLT`，IP 192.168.1.71）✅
- SNTP 校时完成（`开机校时:完成`）✅
- 自启激活成功（test-token 测试组设备）✅
- `音频延后=true`，自启只连 WS 不开音频 ✅
- `WebSocket 已连接`、`服务器握手成功`、hello/session/MCP 握手正常 ✅
- **无 OOM、无 panic、无崩溃、无重启** ✅
- WS 启动前 heap free=70744 / 最大连续块=57344（证书瘦身生效）✅

**剩余：端到端语音路径（mic→opus→server→decode→speaker）需用户进小智页按 OK 说话才能物理验证**（见 4.2 判读表），代码路径已设计为进页 `defer=false` 后初始化音频，但未经真机语音测试。

---

## 0. 一句话状态

| 项 | 状态 |
|---|---|
| 握手/协议（hello、MCP、listen、abort） | ✅ 已修好，服务器侧有正常记录，**真机开机自动连接成功** |
| 下行 TTS（服务器 → 解码 → 扬声器） | ✅ 已验证通（能听见"无人应答"的告别语音） |
| 上行（麦克风 → Opus 编码 → WebSocket） | ⚠️ 根因已定位为**堆内存不足**，修复已落地（常驻模型 + 建栈顺序 + defer-audio），**端到端语音需用户按键说话物理验证** |
| "多按两下就崩溃重启" | ✅ 已定位（opus 任务栈被误裁到 8192 → 爆栈），已改回 24576 |
| 最后一次构建 | ✅ **成功**（放弃自启改动后增量构建通过） |
| 自启模式 | ❌ **已放弃**（2026-09-01 用户拍板）：WiFi 常驻 99KB 致音频连续块不足、解码器 ret-7 |
| 触发方式（现） | 用户手动进 **应用→小智** 页面（show_xiaozhi 释放主页后 init 音频） |
| 真机验证（端到端语音） | ⚠️ 需用户进小智页按 OK 说话（本机无法代按） |

---

## 1. 问题时间线与根因

### 1.1 现象演变

1. 设备能激活、能开始对话、能打断，服务器后台有记录 —— 但设备端**没有 AI 语音**。
2. 重启后停在"就绪，确认键对话"，按确认键只有按键提示音，屏幕无变化。
3. 之后出现"多按键交互两下就崩溃重启"。

### 1.2 真正的根因（堆内存不足，不是协议问题）

无 PSRAM 的 C3 上，进入小智前堆基线只剩 **~69 KB**，原因是：

- **`x509_crt_bundle` 常驻吃掉 67.4 KB**：150 张 CA 的证书 bundle（68983 字节）被 `esp_crt_bundle_attach` 在运行时整包解析进堆并常驻。
- 结果：Opus 编码器需要 **17 KB 连续块**拿不到 → `OPUS_ALLOC_FAIL (-7)` → **上行全哑**。
- 下行能响，是因为解码器在堆还没被挤干时就已分配成功，且告别语音短、不需要上行。

### 1.3 崩溃重启的根因（一条重要教训）

我把任务栈 HWM 日志**读反了**：

- 日志 `栈HWM: ... opus=11840` 是**已用字节**，不是剩余。
- 而且这个读数采集于**编码器 open 失败的期间**（编码路径从未真正运行），所以数值偏低，完全不具备参考价值。
- 据此把 opus 栈裁到 8192 → 编码路径 `celt_encode_with_ec → run_prefilter` 直接 **Stack protection fault**。

对照原版固件 `references/folo-ai-passport-xiaozhi-main`（**同款 C3、同样无 PSRAM**，README 明写），其实测 opus 任务栈为 `2048*12 = 24576`，现已对齐。

> **记住：ESP-IDF 的栈 HWM 打印出来的是已用字节，不是剩余字节。且必须在代码路径真正跑过之后采样才有效。**

### 1.4 用户拍板的方案

**方案 A（激进省内存）**：只信任两张自签根 CA，砍掉整包证书 bundle，净省约 **64.9 KB** 常驻堆。
代价：新增 HTTPS 域名前必须手工补根 CA（见 2.1 与第五节风险）。

---

## 2. 本轮改动清单

### 2.1 证书瘦身（新增文件 + 4 处改动点）

**新增**

| 文件 | 说明 |
|---|---|
| `components/passport_core/certs/roots.pem` | 2549 字节，两张自签根 CA 合并 |
| `components/passport_core/include/passport_certs.h` | 暴露 `passport_certs_root_pem()` / `passport_certs_root_pem_len()` |
| `components/passport_core/src/passport_certs.c` | 实现，引用 CMake 嵌入符号 `_binary_certs_roots_pem_start/end` |

`roots.pem` 内含：

- **DigiCert Global Root G2** ← `api.tenclass.net`（激活 / WebSocket / OTA）
  链：leaf → GeoTrust G2 TLS CN RSA4096 SHA256 2022 CA1 → 此根
- **GlobalSign Root CA (OU=Root CA)** ← `www.baidu.com`（联网探测）
  链：leaf → GlobalSign RSA OV SSL CA 2018 → 此根

提取脚本留在 `E:\bldtmp\extract_roots.py`。

**`components/passport_core/CMakeLists.txt`**

- `SRCS` 增加 `src/passport_certs.c`
- 增加 `EMBED_TXTFILES "certs/roots.pem"`

**4 处 TLS 点由 bundle 改为内嵌 PEM**

| 文件:行 | 用途 |
|---|---|
| `main/main.c:715` | 百度联网探测 |
| `components/passport_xiaozhi/src/passport_xiaozhi.c:586` | OTA check |
| `components/passport_xiaozhi/src/passport_xiaozhi.c:735` | activate |
| `components/passport_xiaozhi/src/passport_xiaozhi.c:862` | WebSocket |

两处 `#include "esp_crt_bundle.h"` 已改为 `#include "passport_certs.h"`。

### 2.2 音频内存架构（对齐原版）

文件：`components/xiaozhi_audio/src/xiaozhi_audio.c`

| 位置 | 改动 |
|---|---|
| `init()` | **建栈顺序（核心，09-01 修复）**：`s_inited=true` → **先建 opus 任务(24576 栈)** → `ensure_encoder_open()` → `set_decode_rate(24000, 60)` 预开解码器 → 最后建 input/output/send 小栈。**必须先建 24KB 大栈再开编解码器**，否则编解码器先分配会切碎堆、opus 栈拿不到连续块（真机实测 `opus=-1`） |
| 行 630–633 注释 | 说明**顺序至关重要**：24KB opus 栈必须在堆最完整时（此时最大连续块最大）拿到，之后编码器 17KB + 解码器 ~10KB 再分配；反过来必 `OPUS_ALLOC_FAIL(-7)` 或 `xTaskCreate 失败` |
| `enable_voice(false)` | **不再** `esp_opus_enc_close`（停麦不释放编码器，避免撞碎片）；只在 `deinit()` 行 697–699 关闭 |
| 行 646–649 | 任务栈最终值（见下） |
| 行 135 / 593 / 651 / 718 / 731 | 新增 `log_heap()` 水位打点：`heap free / min / 最大连续块` |
| 行 527 / 211 | 新增下行首帧、上行首包 dump 日志 |

```c
xTaskCreate(input_task,  "xz_input",  2048,  NULL, 8, &s_input_task);
xTaskCreate(output_task, "xz_output", 2048,  NULL, 4, &s_output_task);
xTaskCreate(opus_task,   "xz_opus",   24576, NULL, 2, &s_opus_task);  /* 对齐原版 2048*12 */
xTaskCreate(send_task,   "xz_send",   3072,  NULL, 3, &s_send_task);
```

### 2.3 握手与交互健壮性

文件：`components/passport_xiaozhi/src/passport_xiaozhi.c`

- `ensure_audio_started()`（行 224）在 WS 启动前调用，**音频先行、网络后行**（修正了旧注释"握手成功后拉起"——实际代码是 WS 前调用）；自启模式下 defer=true 则跳过音频 init，只连 WS。
- 新增 15 s 握手超时 `esp_timer`（`s_hello_timer`，行 49 / 101 / 892）：超时上屏"连接失败，请重试"。
- `WEBSOCKET_EVENT_ERROR` 也上屏（行 425）。
- `toggle_listen` 两条原先**静默 return** 的路径现在都给反馈（行 941–949），屏幕文案只用字库已有字："连接中..." / "连接失败，请重试" / "对话中，请说话"。
- **defer-audio（09-01 新增）**：`passport_xiaozhi_set_defer_audio(bool)` + `s_defer_audio` 标志，自启开机只连 WS、进小智页（主页释放、堆完整）后再 `xiaozhi_audio_init()`，规避开机堆 ~70KB 不足以同时开音频(61KB)+WS TLS(5KB) 的 OOM。

### 2.4 sdkconfig 裁剪（对照原版 C3）

直接改 `sdkconfig`（**注意：改 `sdkconfig.defaults` 对已存在的 `sdkconfig` 不生效**），并同步写回 `sdkconfig.defaults` 兜底：

| 配置项 | 旧 → 新 | 说明 |
|---|---|---|
| `ESP_WIFI_STATIC_RX_BUFFER_NUM` | 10 → **3** | 原版同款 |
| `ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` | 16 → **6** | 原版同款 |
| `ESP_WIFI_RX_BA_WIN` | 6 → **3** | 原版同款 |
| `LWIP_IPV6` | y → **n** | 只用 IPv4，省 ~5 KB |
| `FREERTOS_IDLE_TASK_STACKSIZE` | 1536 → **768** | 省 0.7 KB |
| `MBEDTLS_DYNAMIC_FREE_CONFIG_DATA` | 未设 → **y** | 握手后释放 SSL 配置 |
| `MBEDTLS_CERTIFICATE_BUNDLE` | y → **not set** | **本轮核心**，省 64.9 KB |

（`MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL` 是上者的子项，父项关闭后自动失效，无需单独设置；当前 `sdkconfig` 中已不存在该符号。）

> `CONFIG_BT_CTRL_BLE_MASTER` 保持 **y**：它是控制器"连接"功能开关，关掉后 controller 拒绝可连接广播（`ble_gap_adv_start` 报 530），语音通道 0xA2B0 是 GATT 连接，必需。

### 2.5 静态自检结果（已做，均通过）

在交出构建之前做的四项离线校验，结论都是好的，**不需要再复查**：

| 检查项 | 结果 |
|---|---|
| 全项目残留 `esp_crt_bundle` 调用 | ✅ 无。仅 `passport_certs.h` 注释里提及；`sim/build/`、`backup/` 下的 `sdkconfig.cmake` 是桌面模拟器与备份产物，不进固件 |
| `passport_core/CMakeLists.txt` 嵌入 | ✅ `SRCS` 含 `src/passport_certs.c`，且有 `EMBED_TXTFILES "certs/roots.pem"` |
| `roots.pem` 两张证书本身 | ✅ 均为自签根（subject == issuer）、`CA:TRUE`、2048 bit。有效期：DigiCert Global Root G2 至 **2038-01-15**；GlobalSign Root CA 至 **2028-01-28** |
| **用 `roots.pem` 实测两个域名的 TLS** | ✅ **均握手成功**（含 hostname 校验）：`api.tenclass.net` leaf CN=api.tenclass.net（深圳十方融海）、`www.baidu.com` leaf CN=baidu.com，均为 TLSv1.2 |

> 最后一项是本轮最有价值的一条：**"砍掉 bundle 后 TLS 会不会挂"这一最大风险已被实测排除**，两张根 CA 的选择是正确的。
> 唯一要注意的是 GlobalSign Root CA **2028-01-28 到期**，届时若百度还在用它，需更新 `roots.pem`。

**配置漂移检查**（`sdkconfig` 被 gitignore，别人接手只能靠 `sdkconfig.defaults`）：

- `sdkconfig` 中同时存在 `CONFIG_ESP_WIFI_*`（第 1484/1485/1498 行）与 `CONFIG_ESP32_WIFI_*`（第 2938/2939 行）两份。
- 已确认 **IDF v5.5.3 中 `ESP32_WIFI_` 旧前缀在 Kconfig / 头文件 / 源码里已完全不存在**，项目代码也不引用 → 那两行是 v4→v5 迁移遗留的**死配置项，不生效**。
- 因此 `sdkconfig.defaults` 只写 `ESP_WIFI_` 前缀是完整且正确的；即使删掉 `sdkconfig` 重建，也不会回退到缓冲数 10/16 的老坑。

---

## 3. 构建与刷写 SOP（接手照抄即可）

### 3.1 环境（必须用 PowerShell，**不能用 git bash**）

`export.ps1` 检测到 MSys/Mingw 会直接报错退出。

```powershell
$env:TEMP = "E:\bldtmp"; $env:TMP = "E:\bldtmp"; $env:TMPDIR = "E:\bldtmp"
$env:IDF_BUILD_JOBS = "4"                       # 注意: idf.py -j 4 无效，必须用环境变量
$env:IDF_TOOLS_PATH = "E:\esp\.espressif"
$env:IDF_PYTHON_ENV_PATH = "E:\esp\.espressif\python_env\idf5.5_py3.12_env"
cd "E:\code\code tools\esp-idf-v5.5.3"; .\export.ps1
cd "E:\code\ai passport\o-platform"; idf.py -p COM6 flash
```

- 工具链在 **E 盘** `E:\esp\.espressif`；C 盘 `C:\Users\20372\.espressif` 是已删除的旧副本，**不要指向它**。
- 串口：**COM6**（USB 串行设备）。
- 后台跑 + 日志重定向到文件（PowerShell 输出捕获在本机不可靠，必须 `Out-File`/`>>` 后用 Python 读；日志是 **UTF-16**，直接 `tail` 看到的是乱码）。

### 3.2 四个已经踩过的坑

1. **刷写失败：COM6 被 `serial_capture.py` 占用**
   → 刷前先 `Stop-Process` 杀掉命令行含 `*serial_capture*` 的 python 进程；刷完等 USB 重枚举再重启抓取。
2. **编译器临时目录 Permission denied**
   → 固定 `TEMP/TMP/TMPDIR=E:\bldtmp`（不要用 `E:\tmp`）。
3. **误判"又全量重编了"**
   → 改 sdkconfig 会触发 confgen 重写 `sdkconfig.h` → 全量重编 1603 步，这是**正常的**，不是出错。下一次应是增量。
4. **Windows 文件系统瞬时故障**（本次卡在这里）
   - `flash_log7`：`OSError: SHFileOperationW failed: 0x2`（CMake `inject_requirements` 删临时目录失败）
   - `flash_log8`：`riscv32-esp-elf-ranlib.exe: unable to copy file 'esp-idf\fatfs\libfatfs.a'; reason: Permission denied`
   - 两次**都与代码无关**。处置：直接重跑；若仍失败，先关掉可能锁文件的杀软/资源管理器窗口，或删 `build/` 下对应的 `.a` 后重跑。

### 3.3 判断编译是否真的在跑

- ESP-IDF v5.x 产物扩展名是 **`.obj` 不是 `.o`**（"0 个 .o = 没开始编译"是错的）。
- `find build -name "*.obj" -mmin -2 | wc -l` 看增量，配合 `tasklist | grep cc1`。

---

## 4. 真机验证步骤与日志判读

### 4.1 操作

1. 刷入成功 → 重启串口抓取：`python E:\tmp\serial_capture.py`（写 `E:\tmp\serial_log.txt`）。
2. 设备进 **应用 → 小智**，按确认键说话。

### 4.2 判读表（按日志出现顺序）

| 日志关键字 | 正常应该看到 | 异常含义 / 处置 |
|---|---|---|
| `音频 init 前: heap free=... 最大连续块=...` | 进入小智后堆基线应 **>130 KB**（原来 69 KB） | 若仍 ~69 KB → 证书 bundle 没关掉，检查 `sdkconfig` 里 `MBEDTLS_CERTIFICATE_BUNDLE` 是否真的 not set，并确认已重编 |
| `开编码器前: ... 最大连续块=` | 连续块 **≥ 20 KB** | < 17 KB 必然失败 |
| `编码器: 16000Hz/60ms (帧 960 采样, 输出缓冲 ...B)` | 出现即成功 | `编码器打开失败: ... (-7)` = `OPUS_ALLOC_FAIL` → 堆仍不够，回到上两行看水位 |
| `编码器打开失败` **不出现** | ✅ | — |
| `上行首包: type=... len=... 头8字节=...` | 说话后 1 s 内出现，`len` 非 0 | 不出现 = 上行链路没数据；检查 `enable_voice(true)` 是否被调用、mic 是否有数据 |
| `首次真实编码: opus 栈 HWM(已用)=N/24576 字节` | 开麦后出现，N 为编码路径真实已用字节 | 用于减栈依据：若 N ≤ ~18KB 可安全减到 20KB，≤ ~15KB 可减到 16KB；若 N 接近 24576 则不可减 |
| `音频服务就绪(24k 采集/播放, 编码 16k/60ms/complexity0)` | 出现 | — |
| `下行首帧写入播放器: N 采样(heap free=...)` | 收到 TTS 后出现 | 不出现 = 服务器没下发或 WS 没收到 |
| `解码失败: ...` | **不应出现** | 出现看具体 err；若伴随堆低则是内存问题 |
| `周期水位: heap free=... min=... 最大连续块=...` | `min` 应保持在安全水位（>20 KB） | 持续下滑 = 泄漏 |
| **`Stack protection fault` / 重启** | **不应出现** | 再出现说明 opus 栈还不够，看崩溃 PC 是否又在 `celt_encode_with_ec` / `run_prefilter` |

### 4.3 预期结果

省下的 64.9 KB + opus 栈 24 KB，应使编码器（17 KB）与解码器（~19 KB）能够常驻，上下行都通：能听见 AI 回答，且不再崩溃。

---

## 5. 已知风险与未完成项

### 5.1 风险：只信任两张根 CA

- 若 `api.tenclass.net` 或 `www.baidu.com` **更换证书链**（换到别的根），TLS 握手会失败，表现为"激活失败 / 联网探测失败"。
- 补 CA 的方法：用浏览器导出目标站点的根证书 PEM，追加进 `components/passport_core/certs/roots.pem` 即可（该文件由 CMake 嵌入，改完重编生效）。
- 若将来需要访问更多 HTTPS 域名，务必同步补根，否则会静默握手失败。

### 5.2 未完成

1. **端到端语音真机验证**（唯一物理阻塞项）：开机自启/WS 连接已真机验证通过，但 `mic→opus→server→decode→speaker` 全链路需用户**进小智页按 OK 说话**才能确认（本机无法代按）。判读见 4.2 表。
2. **蓝牙传输（交接文档 4.1 的方案一 / 方案二）** 待用户拍板。现状：
   - `PKG_CTRL` / `PKG_DATA` / `PKG_STATUS` 三个特征只存在于底座参考代码 `references/passport-platform/components/passport_link/`，**o-platform 已裁掉 passport_link，全仓搜不到**。
   - `.pap` 打包逻辑在 `passport_core/passport_package.c`，但无 BLE 通道。
   - 走方案一需新增 `PASSPORT_PACKAGE_KIND_CARD` 并改 `passport_core` 安装逻辑（`.pap` 目前只支持 kind=app/theme，装到 `/passport/apps/<id>`、`/themes/<id>`，**不能直接写 `/passport/` 根目录**）。
3. **首次 git commit**：交接时仓库 0 次提交，2026-09-01 本轮验证通过后已按此文档建议做首次 commit，固化当前可跑状态（见本轮记忆 `2026-09-01.md`）。

---

## 6. 故障决策树（真机验证时对照）

```
刷入后按确认键
├─ 屏幕无变化 / 只有按键音
│   └─ 看串口: 有没有 "连接中..." 对应的 WS start 日志？
│       ├─ 没有 → toggle_listen 静默路径，检查 s_hello_ok / s_running
│       └─ 有 → 看 15s 后是否 "握手超时" → 查 WiFi / TLS / 证书
├─ 能进对话但没有 AI 声音
│   ├─ 有 "上行首包" 吗？
│   │   ├─ 没有 → 上行断: 编码器是否 open 成功？mic 数据？
│   │   └─ 有 → 服务器没回，或下行断: 看 "下行首帧写入播放器"
│   └─ 有 "编码器打开失败 (-7)" → 堆不够 → 回到 4.2 前三行查水位
└─ 崩溃重启
    └─ 看崩溃 PC / 任务名
        ├─ xz_opus + celt_encode_with_ec → 栈不够，加栈
        └─ 其它 → 看是不是堆分配失败后空指针（编码器 open 失败后有兜底判空，理论上不会）
```

---

## 附：本轮相关文档

- `docs/ws-debug-postmortem.zh_CN.md` — WebSocket 调试复盘
- `docs/xiaozhi-audio-design.zh_CN.md` — 音频链路设计
- `docs/porting-pipeline.zh_CN.md` — 移植管线
- 项目记忆：`E:\code\ai passport\.workbuddy\memory\2026-08-31.md`（本轮逐时段的详细操作记录）
