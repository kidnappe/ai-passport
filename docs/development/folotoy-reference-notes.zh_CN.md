# FoloToy ai-passport 官方仓库参考笔记

> 来源：`references/folotoy-ai-passport-latest`（2026-09-01 拉取，commit 4662144）
> 用途：o-platform 后续开发/构建可借鉴的官方做法。非移植目标，仅参考。

## 1. 构建加速（本次关注核心）

**官方对构建加速的唯一方案 = ccache**，没有其他魔法。

- 官方 CI（`.github/workflows/build-firmware.yml`）：
  - `IDF_CCACHE_ENABLE=1` + `CCACHE_DIR=.../.ccache` 持久化缓存
  - 用 `actions/cache` 按 commit 恢复/保存 ccache
- **o-platform 已完整采用**：ccache 4.12.1 已接入（`CCACHE_ENABLE=1`、
  `CMAKE_CCACHE_PROGRAM` 已设），命中率 ~64%。
- 我们的构建慢**不是编译慢**，是 Windows 文件锁（Defender/WSearch/火绒）导致
  `ranlib/ar Permission denied` 反复失败重试。根治需管理员加排除项或停 WSearch，
  当前环境不可行；**build.ps1 用自动重试绕过**（瞬时锁，重试 3-4 次必过）。

## 2. 构建/验证规范（官方做法，可借鉴）

- `idf.py build`/`flash` 只作**增量开发命令**，不作为默认交付方式。
- 官方用 `./tools/validate.sh --firmware` 在**隔离临时构建目录**编译并生成
  `build/FoloToy-AI-Passport-full.bin`（从 0x0 合并镜像），不污染主 sdkconfig。
- 交付前分别报告：`Build / Host tests / Device tests / Unverified`，
  **"编译通过" ≠ "硬件验证通过"** 要分开记录。
- 仓库提交 `dependencies.lock` 固定 Managed Components 版本；改 `idf_component.yml`
  后用 ESP-IDF 5.5.3 重新生成锁文件。

## 3. 环境（官方，Linux 为主，Windows 参考）

- 平台基线：ESP32-C3、8MB Flash、无 PSRAM、ESP-IDF 5.5.3。
- 安装依赖含 `ccache`（Ubuntu/Debian/Fedora/Arch/macOS）。
- Windows：用乐鑫官方 ESP-IDF Tools Installer；WSL2 可 Linux 编译 + 原生 Windows 烧录。
- 大陆下载慢：Gitee 镜像 + `IDF_COMPONENT_STORAGE_URL=https://components-file.espressif.cn`。

## 4. 官方文档地图（AGENTS.zh_CN.md 路由）

| 任务 | 读官方哪份 |
|---|---|
| 任意代码修改 | `docs/development/agent-guide.zh_CN.md` |
| 构建/测试/依赖/分区 | `docs/development/build-and-test.zh_CN.md`、`ble-recovery-compatibility.zh_CN.md`、`sdkconfig.defaults`、`partitions.csv` |
| BSP/引脚/显示/音频/电池 | `docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md`、`components/bsp/include/bsp_pins.h` |
| 环境/工具链 | `docs/development/environment-setup.zh_CN.md` |
| CI/发布 | `docs/development/CI-*.zh_CN.md` + `.github/workflows/` |
| 文档/commit | `docs/contribution/doc-conventions.zh_CN.md`、`commit-and-pr.zh_CN.md` |

## 5. 代码规范（官方，供对照）

- LVGL 非线程安全：LVGL 任务外访问对象必须持 `bsp_lvgl_lock()`。
- 按键回调不得阻塞；慢操作（音频/存储/网络）放工作任务。
- 可测试的状态机/协议/计时/布局与 ESP-IDF/LVGL 解耦，由 host tests 覆盖。
- 删 screen 前必须停止所有访问其 UI 的任务/定时器/回调。
- 禁提交凭证、私钥、设备秘密、个人数据。

## 6. 经验库（docs/experiences，官方社区踩坑沉淀）

无构建加速专文，但有同平台（C3 无 PSRAM）实用经验：
- **PhoenixZHC**：网络音频流内存预算、SoftAP 配网资源预算（无 PSRAM 约束）。
- **Shinku-Chen**：音频压缩权衡（IMA-ADPCM/Opus/MP3 实测容量）、显示刷新与深睡、
  发布流程。
- **Y2Lin**：串口截屏协议 FAP_SCREENSHOT_V1、音量计 UI 平滑/LVGL 池耗尽白屏。

## 7. skills/ 目录（官方）

发布后的 agent 工作流，与构建无关：
- `issue-suggestions`：收集开发者建议成上游 issue。
- `experience-pr`：把可复用经验作为文档 PR 提交。
- `plays-archive`：把已发布应用归档到 `plays/`。

> 结论：官方 docs/skills **没有**比 ccache 更强的构建加速方案。我们已经把
> 官方唯一有效的加速手段（ccache + 缓存持久化）落地为 `tools/build.ps1`。
