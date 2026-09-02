# 移植他人固件功能的工作管线（Porting Pipeline）

> 适用范围：把**任何外部固件**（底座上游、他人固件、完全无关的第三方固件）中的单个功能，
> 移植进 `o-platform/`（ESP32-C3 · ESP-IDF 5.5.3 · LVGL · 240×320 · 8 MB Flash · 无 PSRAM）。
> 每条规则都有真实案例背书（见文末映射表）。

## 项目关系（先对齐事实）

| 外部项目 | 角色 | 与 o-platform 的关系 |
| --- | --- | --- |
| [rvaim/ai-passport](https://github.com/rvaim/ai-passport)（`references/passport-platform`） | **底座**：插件化平台（.pap 包 / BLE 安装 / Lua 运行时 / passport_core·ui·link·runtime） | o-platform 的基线，按 fork-guide 工作流维护（main 同步上游，功能走 `feature/*`） |
| [killhello/ai-pass-port-wifi](https://github.com/killhello/ai-pass-port-wifi)（`references/wifi-provision`） | **移植案例①**：配网（早期 BLE，后被小智热点配网取代） | 保留交互协议重写为 `o-platform/main/ble_prov.c` + `docs/guides/ble_provisioning.html`；**现行配网已改为小智热点配网 [`78/esp-wifi-connect`](https://github.com/78/esp-wifi-connect)（`components/passport_wifi_ap/`），见 `plan-ble-hotspot-provisioning.zh_CN.md`** |
| [zhaohuaxiaoy/folo-ai-passport-voice](https://github.com/zhaohuaxiaoy/folo-ai-passport-voice) | **移植案例②**：语音输入（MIT） | 按本平台分层重组为 `components/passport_voice`（PTT→16kHz ADPCM→BLE 0xA2B0→桌面 companion→火山流式 ASR） |
| [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)（`references/ai-passport`） | **UI 风格参考**（非功能移植源） | 像素风视觉语言（ui_pixel 配色/组件）被 o-platform UI 借鉴；该克隆内的 volume-cop / emotion-ball 是在官方源码上做的**本地实验**，不属于"移植他人固件" |

## 结论

**管线可用，且已被两次真实移植走通**（配网、语音输入），外加一次底座级 fork。
共六个阶段：**选型筛查 → 参考落地 → [桌面验证·可选] → 适配移植 → 编译+真机验证 → 交付沉淀**。
核心思想两条：

1. **主循环 = 编译 + 真机**（已有真机，默认不走桌面验证；桌面三件套仅作可选工具归档，见阶段 2）；
2. **移植 ≠ 搬文件**——donor 代码要按 o-platform 的分层归位重写（见阶段 3 的归位表），移植走样多半死在"直接拷贝"上。

## 管线总览

```
外部固件 ──▶ [0 选型筛查] ──▶ [1 参考落地] ──▶ [3 适配移植] ──▶ [4 编译+真机验证] ──▶ [5 交付沉淀]
  repo/bin      NOTES.md       references/      按平台分层归位       build 0 warning        feature/* 分支
                                                平台规则内重写        真机走查                文档+产物+记忆

  默认不走的支线：[2 桌面验证]（QEMU 观察 bin-only donor / 观感 spike，见阶段 2）
```

---

## 阶段 0 · 选型筛查（半天内出结论）

**入口**：看中某个固件的某个功能。

**动作**：
1. **License 审查**（一票否决项）：donor 必须是开源可引用的（两个 donor 均为 MIT）。
   无 license 或闭源 bin 只允许"行为借鉴"，不允许抄代码/提取资源直接用。
2. **硬件画像对比**（本机基线）：

   | 项 | 本机 (o-platform) | 不匹配时的代价 |
   | --- | --- | --- |
   | SoC | ESP32-C3 (RISC-V, 单核, ~400KB SRAM) | 大 buffer 方案不可用（语音 donor 的"静态环形缓冲零 malloc"正是为此） |
   | Flash | 8 MB；factory 3 MB（**余量仅 ~673 KB**）；appfs 4.94 MB | 大资源必须进 appfs(FAT `/passport`) |
   | 屏 | 240×320 竖屏 SPI，RGB565 **小端存 flash**（BSP 发屏前交换字节） | 资源字节序错→花屏 |
   | 输入 | UP/DOWN/OK 三个 ADC 按键 | 多键交互要重映射（语音 donor 的音量+键 → 映射到 OK/UP） |
   | 字体 | 仅 `passport_ui_font_size` 14/24 两档，中文字库已占 928 KB，**禁止新增字库** | 文案改用两档字号 |

3. **源码可得性**：有源码 → 走本管线主线；只有 .bin → 只走阶段 2 模式 C（QEMU 观察）+ 行为复刻。
4. **依赖面评估**：donor 的 ESP-IDF/LVGL 版本、NimBLE 等第三方组件与本机的差集；**特别注意 donor 自带的系统服务是否与底座已有服务冲突**（如 NimBLE 生命周期归 `passport_link` 独占，移植件不得再起栈）。

**退出条件**：`projects/<feature>/NOTES.md` 一页纸结论——**移植 / 重写 / 放弃**，附理由、预估工作量、donor 仓库地址与 commit。

## 阶段 1 · 参考落地（只读副本）

**动作**：
1. donor 克隆到 `references/<donor>/`，**永远只读**，记录 commit hash。
2. 圈定功能的**文件清单 + 依赖面**：入口函数、算法文件、资源、**伴生工具**。伴生工具和固件同等重要，要一起移植（案例①的配网页 HTML、案例②的桌面 companion/ 中转与悬浮窗）。
3. **参数收割**：把 donor 的颜色、阈值、尺寸、时序、帧长常量**原值**抄进 NOTES.md（如语音的 3200B/100ms 帧、120ms 帧合并）——移植走样多半死在参数上，而参数核对不需要真机。

> ⚠️ 现状缺口：语音 donor（zhaohuaxiaoy）当时**没有克隆进 `references/`**，只搬了代码——
> 导致现在无法 diff 溯源、无法跟上游更新（见"待补强"）。以后必须先克隆再移植。

## 阶段 2 · 桌面验证（可选，默认跳过）

> **工作方式决策（2026-08-30）：已有真机，移植默认不走桌面验证**——从阶段 1 直接进阶段 3，
> 行为验证统一在编译通过后上真机做。本阶段仅作可选工具归档，两种情形才启用：
> 1. **donor 只有 bin**，或想先看原机完整行为 → 模式 C：IDF 5.5 内置 `idf.py qemu monitor`
>    （首次自动下载 qemu-system-riscv32），8 MB 全镜像直接引导；先例 `references/ai-passport/build_qemu/qemu_flash.bin`。
> 2. **用户明确要求先验证观感/手感** → 模式 B：最小 SDL spike（≤3 个原语回答一个问题）。
>
> 模式 A（单源双编译）的资产已归档：`projects/volume-cop/simulator/`（compat/ 桩层放 include
> 首位顶掉 ESP-IDF/BSP + SDL2 测试台）、`projects/emotion-ball-proto/`，需要时复活。

## 阶段 3 · 适配移植（进 o-platform）

### 3a. 按平台分层归位（两次移植的共同做法，移植不走样的关键）

**不整仓搬、不保留 donor 的目录结构**，把 donor 的每块代码归位到 o-platform 的对应层。
案例②（语音输入）的归位表：

| donor（folo-ai-passport-voice） | o-platform 归位 | 归位理由 |
| --- | --- | --- |
| `main/ble_audio.c`（GATT 0xA2B0 传输） | `components/passport_link/`（voice_ble.c） | **NimBLE 生命周期归 passport_link 独占**，移植件不得自起协议栈 |
| 音频管线 / ADPCM / 协议编解码 / 提示音 | `components/passport_voice/`（全部 worker 化） | 可复用业务逻辑进组件；阻塞操作 worker 化 |
| 视图状态机、按键映射 | `main/main.c`（LVGL 只在 system_task 触碰） | 页面/状态机归 main，LVGL 非线程安全 |
| `companion/` 桌面端（relay、悬浮窗） | 平台外独立交付 | 伴生工具属移植范围，但要脱离固件仓库 |
| 零动态分配、静态环形缓冲 | 保留 | 正面满足本机无 PSRAM/SRAM 紧张约束 |

### 3b. 平台硬规则（违规必返工，详见 `o-platform/AGENTS.zh_CN.md`）

- 页面删除 screen 前先停定时器/任务/回调；退出路径必须 `passport_ui_page_destroy`（先例 bug：transfer_page 二次进入白屏）。
- LVGL 非线程安全，任务侧访问必须 `bsp_lvgl_lock()`；按键回调只投递事件，慢操作进工作任务。
- 资源落位：持久数据走 `/passport`（appfs FAT）；flash 内资源先对 673 KB 余量做预算。
- 颜色/图片：flash 里的 .raw 一律**小端 RGB565**。
- UI 视觉遵循官方固件风格（FoloToy 像素风语言），不引入 donor 的视觉体系。

### 3c. License 合规

文件头保留出处注释（先例：`passport_voice.h` 头部"移植自 folo-ai-passport-voice"、`voice_ble.c` 头部"移植自 main/ble_audio.c"），汇总进 THIRD-PARTY-NOTICES。

## 阶段 4 · 编译+真机验证（默认主循环，全过才算完）

```powershell
# 构建环境（必须 PowerShell；git bash 会被 export.ps1 拒绝）
$env:IDF_PYTHON_ENV_PATH = "C:\Users\20372\.espressif\python_env\idf5.5_py3.12_env"
cd "E:\code\code tools\esp-idf-v5.5.3"; .\export.ps1
cd "E:\code\ai passport\o-platform"; idf.py build          # 门禁1: 0 warning
idf.py size-components                                      # 门禁2: 对比 flash 预算
idf.py flash monitor                                        # 门禁3: 上真机
```

- 门禁3：**真机场景走查（行为验证的主战场）**——音频、ADC 按键、BLE 实连、屏幕字节序这些只有真机能验；donor 自带 host 测试的（如语音 donor 状态机 ctest）可先在 PC 跑通作基线。
- 门禁4：**cleanup pass**：搜未引用旧符号→删重复 helper→清 include→重跑测试。
- 门禁5：`tools/validate.sh --static`；伴生网页用 node 冒烟（先例：`tools/check_transfer_rgb565.js`，vm 沙箱抽 HTML 内脚本验证字节序）。

## 阶段 5 · 交付沉淀

- `feature/<name>` 分支 + PR；**main 永远保持与底座上游同步**，不混功能（`o-platform/docs/fork-guide.zh_CN.md`）。
- 双语文档（`.md` + `.zh_CN.md` 成对），素材进 `docs/assets/`。
- 可执行产物收进 `projects/<feature>/artifacts/`（先例：refresh_artifacts.ps1 模式）。
- 更新 `.workbuddy/memory/`：把本案例踩的坑回写成长期记忆，供下次阶段 0 直接检索。

---

## 真实案例对管线的验证

| 案例 | donor | 走到的阶段 | 关键做法 | 证据 |
| --- | --- | --- | --- | --- |
| 底座 fork | rvaim/ai-passport | 0→5（持续） | fork-guide：main 同步上游、功能走 feature/* | `o-platform/` 全仓 |
| ① 配网（早期 BLE，后被小智热点配网取代） | killhello/ai-pass-port-wifi | 0→1→3→4→5 | 保留交互协议+伴生网页，代码按本平台重写 | `main/ble_prov.c`、`docs/guides/ble_provisioning.html` |
| ② 语音输入 | zhaohuaxiaoy/folo-ai-passport-voice (MIT) | 0→1→3→4→5 | **按平台分层归位**（link/voice/main 三层拆解）、伴生 companion 独立交付 | `components/passport_voice/`（出处注释在 passport_voice.h、voice_ble.c） |
| （旁证）桌面验证技术 | FoloToy 官方源码上的本地实验 | 仅阶段 2 工具预演 | 单源双编译、SDL spike、QEMU 引导 | `projects/volume-cop/`、`projects/emotion-ball-proto/`、`build_qemu/` |

> 注：volume-cop / emotion-ball 是在官方固件克隆上做的风格与手感实验（UI 风格参考的一部分），
> **不是移植案例**；但它们预演了阶段 2 的三种桌面验证模式，技术资产直接入库复用。

## 待补强（提升下次效率）

1. **语音 donor 未入库**：克隆 zhaohuaxiaoy/folo-ai-passport-voice 到 `references/` 并记录移植时基线 commit，否则无法 diff 溯源、无法跟上游修复。
2. QEMU 镜像刷新没有脚本 → 补一个 `refresh_qemu.ps1`（build_qemu → 拼全镜像）。阶段 2 降为可选后，QEMU 是观察 bin-only donor 的主要手段，此脚本优先级上升。
3. （归档资产，优先级低）`compat/` 桩头文件每个项目复制一份 → 需要复活桌面验证时再抽成 `tools/sim-compat/` 共享层；`projects/volume-cop/simulator/CMakeLists.txt` 的 `AI_MAIN` 路径也已漂移（donor 现位于 `references/ai-passport/`）。
4. bin-only donor 的资源提取流程尚未实操，目前只有 QEMU 观察经验。
5. emotion-ball 尚未从 spike 推进为正式功能，可作为管线下一次完整演练的候选（对照官方风格做归位移植）。
