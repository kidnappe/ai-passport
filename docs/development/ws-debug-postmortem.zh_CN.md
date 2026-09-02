# 小智 WS 连接调试复盘（错误 · 原因 · 应对）

> 适用场景：ESP32-C3 固件（o-platform）接入小智（xiaozhi）官方 WebSocket 服务时，
> 出现「握手后约 100ms 被服务器 CLOSE」的问题。本文记录整个排查过程中
> **所有错误做法、错误原因、以及对应的应对方法**，供后续（尤其音频管线）直接复用。
>
> 关联文档：`docs/porting-pipeline.zh_CN.md`

---

## 0. 结论速览

| 项 | 内容 |
|---|---|
| **现象** | WS 握手 101 成功 → 收到服务器 hello → 立即 `ConnectionClosed`；真机日志 `连接已断开` + `poll_connection_closed: unexpected data readable` |
| **真因（固件侧）** | `esp_websocket_client_set_headers()` 在 `start()` **之前**调用且**返回值被忽略** → 静默返回 `ESP_ERR_INVALID_ARG` → `Device-Id`/`Client-Id`/`Authorization`/`Protocol-Version` 全没进握手报文 |
| **为什么被关** | 服务器 101 只是 HTTP upgrade 放行，并不等于鉴权通过；认不出设备，发完 hello 后直接 `CLOSE` |
| **解法** | 鉴权头改在 `esp_websocket_client_init()` 时经 `ws_cfg.headers` 传入；每次 start 前 destroy 重建客户端；`stop()` 改 destroy + 置 NULL |
| **决定性证据** | PC 端 A/B 对照（同 URL/token/hello，只差一组头）：不带头 → 101 后被关（code=1005，与真机一致）；带头 → 回 `hello`+`session_id`+`mcp/initialize`，连接保持打开 |

---

## 1. 错误做法全清单

按「错误做法 → 为什么这么想 → 实际真相 → 应对」四栏拆解。

### A 组：方向性误判（连接秒关的锅甩错了地方）

#### 错误 1：服务器 test-token「死态」说
- **错误做法**：在服务器侧排查，认定官方小智服务器对 test-token 设备故意发 `CLOSE`，是服务器端死态/限流。
- **为什么这么想**：用 test-token 连上去，握手 101 成功，但收到 hello 后立刻被关，直觉往「服务器不认这个 token/设备」上靠。
- **实际真相**：服务器根本没收到能识别设备的头（全缺），101 只是 upgrade 放行、不等于鉴权通过；认不出设备才在 hello 后 `CLOSE`。
- **应对**：先看**自己发出去**的握手报文，再判服务器死活；不要因为 token 名字带 `test` 就锚定。

#### 错误 2：BLE 干扰说（ble_guard / ble_yield 两轮）
- **错误做法**：查 BLE 守卫逻辑、BLE yield 是否抢占/干扰了 WS 连接，怀疑 BLE 常驻把堆/CPU 占掉导致 WS 断。
- **为什么这么想**：C3 无 PSRAM，BLE 常驻本身吃内存，「连接断」看着像资源被抢；且 BLE 还服务语音通道，条件反射往 BLE 上找。
- **实际真相**：WS 走 WiFi 协议栈独立通道，与 BLE 跑不跑、让不让 CPU 无因果关系；秒关纯粹是握手头缺失。
- **应对**：先确认故障域（WS 栈 vs BLE 栈），用进程/任务/日志隔离，不要「看着像资源问题就查资源」。

#### 错误 3：组件生命周期说（lifecycle / xz_after_lc 两轮）
- **错误做法**：怀疑小智组件 start/stop 生命周期管理有问题——重复 start、stop 不干净、状态机没复位，导致连接状态异常。
- **为什么这么想**：日志里「连接已断开」反复出现，且进/出小智页会反复 start/stop，容易归因到生命周期。
- **实际真相**：生命周期本身没问题；连接每次都断，原因始终在握手那一步缺头，跟 start/stop 几次无关。
- **应对**：用「最小复现」验证生命周期假设——若单 start 一次也断，就不是生命周期问题。

### B 组：根因层错误（最致命、最隐蔽）

#### 错误 4：`set_headers` 调用时机错误 + 忽略返回值 ⭐ 真正死因
- **错误做法**：在 `esp_websocket_client_start()` **之前**调用 `esp_websocket_client_set_headers(s_ws, headers)`，且不检查返回值。
- **为什么是错的（技术根因）**：没读源码约束。该函数内部
  ```c
  if (client->state != WEBSOCKET_STATE_CONNECTED) return ESP_ERR_INVALID_ARG;
  ```
  start 之前 state 还不是 `CONNECTED`，直接返回错误码；固件把返回值丢了，以为头设上了。
- **后果**：握手报文里从头到尾没有 `Device-Id`/`Client-Id`/`Authorization`/`Protocol-Version`
  （握手头在 `transport_ws.c` 拼 `ws->headers` 时才生效，而它一直是空）→ 服务器认不出设备 → hello 后 `CLOSE`。
- **应对**：
  1. 鉴权头改在 `init()` 时经 `ws_cfg.headers` 传入（头只在 init 生效）；
  2. 每次 start 前 destroy 重建客户端（token 也能动态更新）；
  3. `stop()` 改 destroy + 置 NULL，退出小智页释放 4KB rx + 4KB tx（C3 无 PSRAM）；
  4. **调用任何第三方库 API 后检查返回值**，尤其 esp-idf 这套。

#### 错误 5：误读 `unexpected data readable`（把果当因）
- **错误做法**：看到 `poll_connection_closed: unexpected data readable` 和「连接已断开」，认为是服务器发了「异常数据」把连接搞断，继续在服务器/协议层找茬。
- **为什么这么想**：日志字面意思是「收到意外可读数据」，自然往「对方发了不该发的东西」联想。
- **实际真相**：**因果反了**。客户端自己先决定进 `CLOSING`（认不出设备、收到 hello 后处理异常），不再读 socket；服务器先发的 hello+mcp 帧残留在接收缓冲没被读走，close 时 poll 到这些残留才报「unexpected readable」。这条日志是**结果，不是原因**。
- **应对**：日志里的 `unexpected/error` 要把因果分清——先问「是谁先关的、为什么关」，别把残留在缓冲的数据当成「对方攻击」。

### C 组：修好连接、补 MCP 时的两个小错

#### 错误 6：对 `notifications/initialized` 误回 error
- **错误做法**：第一版把服务器发的 `notifications/initialized`（通知，无 id）当成请求，回了 error。
- **为什么错的**：没区分 JSON-RPC 请求和通知——通知以 `notifications/` 前缀标识、没有 id、不要求回复。原版 `mcp_server.cc:365` 直接以前缀忽略。
- **修正**：method 以 `notifications` 开头且 id 缺失 → 直接忽略，不回。
- **应对**：实现任何 RPC/协议时，先区分「请求（需回复）」与「通知（单向）」，再落码。

#### 错误 7：注释里写 `notifications/*` 触发 C 注释提前结束
- **错误做法**：代码注释里写了 `notifications/*` 描述通知前缀。
- **为什么错的**：C 注释 `/* ... */` 里出现 `*/`（即使是 `/*` 的一部分）会被预处理器当成注释结束，后面代码被吞 → 编译失败。
- **修正**：注释改成「notifications 前缀」之类不带 `*/` 的字样。
- **应对**：注释/字符串里避免写出 `*/`、`/*` 这种会闭合注释的字符序列。

---

## 2. 为什么一开始没锁定真因？

真因是一个「静默失败」，且所有可见症状都指向别处——既没报错，又把锅天然甩给服务器和无关子系统，导致最便宜的定位手段一直没用上：

1. **没有报错，等于没有线索**：`set_headers()` 返回了 `ESP_ERR_INVALID_ARG`，但被忽略。无崩溃、无「headers 失败」日志，缺陷完全哑火。
2. **症状天然外归因**：101 成功 + 服务器关连接 → 直觉是「服务器不要我们」。真问题在我们**发出去的请求**，却一直盯着**收到的响应**。
3. **从没看过自己发出去的是什么**：早点 dump 握手报文，`Device-Id`/`Authorization` 全缺会立刻跳出来。缺陷在 outbound，注意力在 inbound。
4. **被 `test-token` 锚定**：token 带 `test`，先入为主「服务器对测试设备死态」，之后所有排查都在帮它找证据而非证伪。
5. **最便宜的隔离实验（A/B 带/不带头）做太晚**：一次就能把「客户端 vs 服务器」切开，却排在 BLE/生命周期/协议好几轮之后。
6. **最该早读的那行源码读太晚**：铁证 `if (state != CONNECTED) return INVALID_ARG` 就在 `esp_websocket_client.c` 里，一次 grep 的事，却最后才看。
7. **`unexpected data readable` 被当成因**：看着像「服务器发了异常数据」，把人引向服务器/协议层，又加深「问题在对方」的错觉。

**本质**：无症状（静默失败）+ 症状外指（锅在服务器）+ 假设锚定（test-token 死态）三重叠加，让「看发出去什么 / 读 API 源码 / 做 A/B」这三件最便宜的事全被排到最后。等真去做了 A/B，真因一击即中——说明它不是难找，是被前面的噪音盖住了。

---

## 3. 是否都因「没好好参考小智固件源码」？

**不完全是，要分两层。**

- ✅ **核心那个 bug 是**：WS 鉴权头设置方式写错了。donor 小智固件是能正常工作的 xiaozhi 客户端，必然以正确方式把头送进握手；我们**没把 donor 的 WS 初始化整段搬过来，而是自己手写了一版**才写出时序错误。忠实照抄 donor 的 WS 初始化就能直接避开这条缺陷。→ 你的「参照原版、别乱参照」针对的正是这一层，正确。
- ❌ **那些绕远路的误诊不是**：BLE/生命周期/服务器死态三个方向性误判，根子在**调试纪律**（失败静默、症状外归因、没做最小隔离），不靠 donor 参考也能犯，照抄 donor 也未必能避免。
- 🔧 **真正破案的两招也不依赖 donor**：① 读 esp-idf 自己的 `esp_websocket_client.c` 那行约束；② PC 端 A/B 对照。这是通用方法。

**准确说法**：「没好好参考小智固件」解释了**缺陷**（手写错 WS 初始化），但解释不了**绕路**（那些误诊靠更好的调试纪律才能避免）。

---

## 4. 通用应对方法（防再犯 Checklist）

落地为可执行的纪律，下次（含音频管线）直接照做：

- [ ] **第三方库 API 静默失败 → 先读源码约束**：调用前 grep 该函数实现，确认前置条件（state、调用时机、是否需要已连接）。esp-idf 的约束常藏在返回值里。
- [ ] **忽略返回值 = 埋雷**：esp-idf 这套返回值几乎都该查；`ESP_ERR_INVALID_ARG`/`ESP_FAIL` 出现就是信号。
- [ ] **现象异常先做最小 A/B 隔离**：只改一个变量（如带/不带某组头），一次把「客户端 vs 服务器」「本模块 vs 其他模块」切开。
- [ ] **先看自己发出去什么**：网络问题先 dump 实际发出的报文/字节，再判对端死活。缺陷常在 outbound。
- [ ] **分清日志因果**：`unexpected/error` 类日志先问「谁先关的、为什么关」，别把残留缓冲数据当「对方攻击」。
- [ ] **不锚定早期假设**：token 带 `test`、日志带某关键词，都不构成结论；尽早用实验证伪。
- [ ] **协议实现先分请求/通知**：RPC/JSON-RPC 类，先区分「需回复的请求」与「单向通知（如 `notifications/*` 前缀、无 id）」再落码。
- [ ] **注释/字符串避开 `*/`、`/*`**：防止 C 注释被提前闭合导致编译失败。

---

## 5. 对音频管线（Opus 编解码 + 收发）的落地要求

用户明确要求：**参照原版小智固件（donor = folo-ai-passport-xiaozhi，基于 78/xiaozhi-esp32 v2.4.2），别乱参照，争取一次性实现可对话。**

两个动作缺一不可：

1. **照抄 donor 的工作模式（防「写出 donor 早修好的 bug」）**
   - 音频参数：16kHz / 单声道 / 60ms 帧 / Opus `complexity=0`（来自 donor `audio_service.cc`）。
   - 上行帧：协议 v3 二进制帧 `type=0`，结构 `type(1B) + reserved(1B) + payload_size(2B 大端) + payload`（来自 donor `websocket_protocol.cc` / `protocol.cc`）。
   - Opus 编解码：donor 用 `espressif/esp_audio_codec`（`main/idf_component.yml` 已声明依赖）；ESP32-C3 上可用。
   - 采集/播放任务循环、codec 读写抽象、LiteAudioEngine 直通（C3 无 AEC/VAD）整体照搬 donor 结构。
   - 依赖声明：在 `passport_xiaozhi` 的 `idf_component.yml` 补 `espressif/esp_audio_codec` 等，与 donor 对齐。

2. **保留本次学到的调试纪律（防「又绕七八圈才定位」）**
   - 改完先 dump 实际发出的 WS 二进制帧，确认 `type=0` 与 payload 长度正确。
   - 调用 Opus/codec API 后逐条检查返回值，不忽略。
   - 录音→编码→上传、接收→解码→播放任一段异常，先用最小 A/B（如先发静音帧、先只解码不播放）隔离是哪一段。
   - 内存：C3 无 PSRAM，Opus 缓冲与音频环形队列尺寸需按 donor 量级控制，避免与 BLE 常驻堆叠加触发 panic。

---

## 附：关键源码定位（供速查）

| 位置 | 内容 |
|---|---|
| `managed_components/espressif__esp_websocket_client/esp_websocket_client.c` (~L1020) | `set_headers` 的 `state != CONNECTED → ESP_ERR_INVALID_ARG` 约束 |
| `esp-idf/components/tcp_transport/transport_ws.c` (~L273) | 握手报文在拼 `ws->headers` 时才带入头 |
| `references/folo-ai-passport-xiaozhi-main/main/mcp_server.cc` (~L365, L384) | MCP `notifications` 前缀忽略、`initialize` 回复 |
| `references/folo-ai-passport-xiaozhi-main/main/audio/audio_service.cc` | Opus 参数、采集/编码/解码/播放任务循环 |
| `references/folo-ai-passport-xiaozhi-main/main/protocols/websocket_protocol.cc` | 二进制音频帧 `SendAudio` 与收包解析 |
| `references/folo-ai-passport-xiaozhi-main/main/protocols/protocol.cc` | 协议帧封装 `type/reserved/payload_size/payload` |
