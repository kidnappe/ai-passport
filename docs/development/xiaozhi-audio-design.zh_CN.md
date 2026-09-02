# 小智音频管线移植：确定的设计方案与注意事项

> 状态：**设计已定稿，实现进行中**（2026-08-31）
> 参照物（donor）：`references/folo-ai-passport-xiaozhi-main` —— 这块 ESP32-C3 工牌的
> 官方定制版小智固件（基于 78/xiaozhi-esp32 v2.4.2）。**所有行为参数以 donor 在本板上的
> 实际运行为准，不从上游 xiaozhi-esp32 通用代码推断，更不自己发明。**
>
> 关联文档：`docs/ws-debug-postmortem.zh_CN.md`（WS 调试复盘，本设计的调试纪律来源）、
> `docs/porting-pipeline.zh_CN.md`（移植管线总则）

---

## 0. 已敲定的决策速览

| 决策点 | 结论 | 依据（donor 源码位置） |
|---|---|---|
| 目标 | 进小智页 → 按键对话 → 语音上行 → AI TTS 播放，一次打通 | — |
| 音频参数 | 16kHz / 单声道 / Opus 60ms 帧 / complexity=0 / VBR / DTX | `audio_service.h` `AS_OPUS_ENC_CONFIG()` |
| I2S 采样率 | 24kHz（输入=输出） | `boards/folo/ai-passport-c3/config.h` `AUDIO_*_SAMPLE_RATE 24000` |
| 上行帧格式 | 协议 v3：`type(1B) + reserved(1B) + payload_size(2B 大端) + payload`，type=0 | `websocket_protocol.cc` `SendAudio` |
| 队列上限 | 发送 40 包 / 编码 2 帧 / 解码 20 包 / 播放 2 帧，**不裁剪** | `audio_service.h` L40-43 宏 |
| 内存模型 | 逐包动态分配（donor 的 deque+vector 语义），**不用静态预分配环** | `audio_service.cc` 队列实现 |
| 任务结构 | audio_input(prio8/4KB) / audio_output(prio4/2KB) / opus_codec(prio2/24KB)，send 环节并入组件 | `audio_service.cc` `Start()` |
| 对话模式 | **auto 模式半双工**：AI 说话时显式关麦；按键 abort 打断 | `application.cc` L1027 + `GetDefaultListeningMode()` |
| 唤醒词 | **不移植**——donor 在本板出厂默认即 `WAKE_WORD_DISABLED`（这是原版行为，非阉割） | `Kconfig.projbuild` L158-188 |
| AEC/VAD | 不移植——`USE_AUDIO_PROCESSOR`/`USE_SERVER_AEC` 依赖 S3/P4/S31+PSRAM，C3 开不了 | `Kconfig.projbuild` L229-243 |
| Opus 实现 | `espressif/esp_audio_codec ~2.5.0`（与 donor `idf_component.yml` 同版本） | `main/idf_component.yml` |
| 重采样 | `esp_ae_rate_cvt`（同库），24k↔16k 复杂度 2 / 速度优先 | `audio_service.cc` `RATE_CVT_CFG` |

---

## 1. 架构（C 版 AudioService → 组件 `components/xiaozhi_audio`）

```
上行:  MIC(24k I2S) ─ audio_input 任务(10ms 读) ─ 重采样24k→16k ─ 60ms 组帧
        ─ 编码队列(≤2) ─ opus 任务(编码) ─ 发送队列(≤40) ─ send 任务
        ─ sender 回调(passport_xiaozhi 组 v3 帧 → WebSocket)

下行:  WS 二进制帧 ─ xiaozhi_audio_push_decode() ─ 解码队列(≤20)
        ─ opus 任务(解码 + 重采样16k→24k) ─ 播放队列(≤2) ─ audio_output 任务
        ─ bsp_audio_write(阻塞写 = 自然实时节流)
```

与 donor 的任务/优先级一一对应（`AudioInputTask`/`AudioOutputTask`/`OpusCodecTask`），
新增 send 任务是因为 donor 的发送在其主循环（event group 驱动）里做，我们没有常驻主循环，
由组件自带发送任务消费发送队列。

### 实现差异清单（仅 3 条，均有 donor 依据）

1. **唤醒词/AEC/音频测试不移植**：本板出厂默认即关（见上表）。LiteAudioEngine 在无唤醒词
   配置下就是"直通引擎"——缓存到 60ms 整帧交编码队列，我们直接实现直通部分。
2. **Opus 编/解码器错峰打开**：donor 在 Initialize 时两个都开（~17KB+~13KB 常驻）；
   我们开麦时开编码器、停麦关，首个 TTS 包开解码器、退出页面关。依据：auto 模式半双工下
   donor 自己在 Speaking 态也停上行（`EnableVoiceProcessing(false)`），两者从不同时需要。
   峰值内存从"之和"降为"取最大"。
3. **每包 payload 512B 逻辑帽**（donor 的 vector 无上限）：60ms Opus 自动码率实测 150-350B，
   不会触顶；触顶打日志丢包，不静默。防异常大包拖垮堆。

---

## 2. 内存方案（本设计最重要的讨论结论）

**结论：照抄行为与上限，不照抄"内存够用"的假设；用动态分配 + 优雅降级 + 真机实测兜底。**

### 2.1 为什么队列上限不裁（曾裁过，已撤回）

我们固件堆基线比 donor 低（NimBLE 静态池、FAT/appfs、工牌 UI——"看不见的占用"），
曾因此把队列裁成 16/12/3，这是**错的**：

- donor 的 40/20 是 `std::deque` 动态队列的**上限**，不是内存承诺；平时队列接近空。
- 静态预分配才会让上限=常驻（40 包 ≈ 20KB+），那是我先选错内存模型倒逼出的裁剪。
- 裁上限损失 donor 验证过的抗抖动窗口（上行 2.4s / 下行 1.2s），却对低基线毫无帮助
  ——低基线时队列本来就到不了 40。

### 2.2 动态分配为什么恰好是对"看不见的占用"最安全的

| 内存模型 | 内存不够时的表现 |
|---|---|
| 静态预分配 | 初始化失败 / panic —— **硬崩** |
| 逐包动态分配 | malloc 返回 NULL → **丢这一帧 + 打日志**，对话继续，仅抖动缓冲变小 |

未知占用被自动挤压为音质降级而非崩溃。40/20 上限只是天花板，实际挂多少包由可用内存决定。

### 2.3 真正的硬承诺（要盯的就这几项）

| 项 | 大小 | 说明 |
|---|---|---|
| opus_codec 任务栈 | 24KB | donor C3 实际值（`2048*12`）。栈不能砍——栈溢出是崩溃，比堆紧张更糟 |
| audio_input / output 任务栈 | 4KB / 2KB | donor C3 实际值 |
| Opus 编码器状态 | ~17KB | 开麦期间持有 |
| Opus 解码器状态 | ~13KB | TTS 播放期间持有；与编码器错峰，不同时在 |
| I2S DMA | ~5.6KB | 已存在（bsp_audio，与工牌语音共用） |
| 重采样器 ×2 | 各 ~1-2KB | 24k→16k 开麦时；16k→24k 解码时 |
| BLE | 小智页期间已停 | 实测释放 ~47KB 堆（xiaozhi_ble_guard），静态池不可还但堆大头能还 |

### 2.4 防线（真机实测按数据调整，不纸面猜）

1. **heap 水位日志**：对话期间周期打印 `esp_get_minimum_free_heap_size()`；
2. **丢帧/分配失败日志**：每次 malloc 失败或队列丢包留痕，绝不静默；
3. 实测水位紧张 → 按**数据**收队列上限或降任务栈，每次改动单独验证。

---

## 3. 对话状态机（对齐 donor application.cc，C 版）

```
idle ──按键──> connecting(WS+hello) ──> listening(auto 模式, 开麦+listen start mode=auto)
listening ──服务器 VAD 断句──> speaking(tts start: 关麦, 解码播放)
speaking ──tts stop──> listening(重开麦, auto 连续对话)
speaking ──按键 abort──> 发 {type:abort}, reset_decoder, 回 listening
任意态 ──WS 断开──> idle(显示"连接已断开", 按键重连)
```

- `listen start` 带 `"mode":"auto"`（`protocol.cc` `SendStartListening`）；
- 服务器 hello 的 `audio_params.sample_rate/frame_duration` 解析存下，喂给解码器
  （`websocket_protocol.cc` `ParseServerHello`）；默认 24000/60（`protocol.h` 初值）——
  注意这是**服务器下发**的解码参数，与我们 hello 里声明的上行 16k/60 无冲突；
- `tts sentence_start` 的 text 刷状态栏（已有）；`tts stop` 后 auto 模式回 listening。

## 4. 与现有代码的接合

| 接触点 | 约定 |
|---|---|
| `passport_xiaozhi.c` | 持有状态机；注册 sender 回调（组 v3 帧发送）；WS 事件里二进制帧调 `xiaozhi_audio_push_decode`；tts/abort 事件驱动开关麦 |
| `main.c` 小智页 | OK 键 = toggle 对话（idle→连接触发对话 / listening→abort 打断 / speaking→abort 打断）；进页面 start、退页面 stop+deinit（服务不常驻，已有生命周期约定） |
| `bsp_audio` | 小智页独占使用（24k 全双工 I2S）；与工牌语音（passport_voice）靠页面互斥，绝不并发 |
| BLE | 小智页期间保持停止（xiaozhi_ble_guard 已有），退出不自动恢复（语音页按需启动） |
| WS 客户端 | 每次连接 destroy 重建（上次 postmortem 的修复保持不变）；音频发送全部走 `esp_websocket_client_send_bin`，返回值必查 |

## 5. sdkconfig 变更（全局性改动，单独验证）

采用 donor 的 C3 内存调优（`sdkconfig.defaults.esp32c3`）：
`ESP_WIFI_STATIC_RX_BUFFER_NUM=3`、`DYNAMIC_RX=6`、`RX_BA_WIN=3`、`LWIP_TCPIP_RECVMBOX_SIZE=16`、
`LWIP_IPV6=n`、`WPA3_SAE=n`、`ESPNOW_MAX_ENCRYPT_NUM=0`、`FREERTOS_IDLE_TASK_STACKSIZE=768`、
`MBEDTLS_DYNAMIC_FREE_CONFIG_DATA=y`。

⚠ 这项影响 WiFi 全局行为（配网/重连/传输页），刷机后需回归：自动重连、热点配网、传输页上传。
路由器若只有 WPA3 需注意 `WPA3_SAE=n` 的兼容性（实测设备 WiFi 为 CMCC 常规 WPA2，可用）。

## 6. 验证计划（按序，每步过再下一步）

1. 编译通过（关注 flash 超分区：factory 3MB 已用 67%，esp_audio_codec 预计 +100-200KB，够）；
2. 刷机抓启动日志：任务创建、codec 打开 24k、编码器懒开；
3. 进小智页：WS 连接 → hello → **上行首包 dump（type=0, len, 前 8 字节）**；
4. 按键说话：服务器回 `stt` 文本 = 上行链路通；
5. AI 回话：`tts start` → 解码播放出声 → `tts stop` → 自动回 listening = 全链路通；
6. 连续对话 3 轮 + 按键打断 1 次，全程 heap 水位日志无逼近 0、无丢帧风暴、无 panic；
7. 退出页面：任务销毁、编码器全关、堆回收至进页前水平。

## 7. 注意事项（含踩过的坑，防再犯）

- **所有 esp_audio_codec / rate_cvt / bsp_audio / WS API 返回值逐条检查**——上次 WS 头静默
  失败的教训；`ESP_ERR_INVALID_ARG` 出现就是信号。
- **首包必 dump**：上行帧头 `type/reserved/len` 与字节序（len 大端）肉眼可验。
- **`bsp_audio_set_format` 的坑**：已打开时不重配采样率，须 close→重 open（16k↔24k 切换场景）；
  编解码全走 24k 后实际不切，但别依赖这个行为。
- **注释里禁写 `*` `/` 连排**（postmortem 错误 7，`-Werror=comment`）。
- **esp_codec_dev 的 `no_dac_ref=true`**：单声道纯录音必须为 true，否则录到 DAC 参考恒为 0
  （bsp_audio 已处理，勿动）。
- **I2S 时钟寄存器不要手动覆写**（REG01~06）：驱动已按采样率算好，覆写=全杂音。
- **`esp_websocket_client_send_bin` 可阻塞**：只在 send 任务上下文调用（组件已保证），
  不在 WS 事件回调里发（回调在 ws 客户端任务里，会死锁）。
- **PCM 缓冲对齐**：编码器要 `frame_size` 整数倍（960 采样 @16k/60ms）；采集 10ms(160 采样)
  累积到 960 再进编码队列，残余样本留在跨帧缓冲。
- **解码器采样率变更**：服务器 hello 参数与默认不同时须关旧开新（donor `SetDecodeSampleRate`
  同款逻辑），不能只改配置。
- **页面生命周期**：小智页退出必须 `xiaozhi_audio_deinit()`（还 24KB 栈+编解码器状态），
  C3 无 PSRAM，残留即配网/BLE 场景的 panic 温床。
