# 项目文件夹结构整理说明

> 整理日期：2026-08-30

## 一、清理（删除）

| 文件 | 原因 |
|------|------|
| `boot_log.txt` | 临时串口日志 |
| `build_sim.cmd` | 模拟器构建命令，已归入 `sim/` |
| `CLAUDE.md` / `CLAUDE.zh_CN.md` | 已被 `AGENTS.md` 替代 |
| `HANDOVER.md` | 已归档至交接文档 |
| `find_show_wifi.py` | 一次性诊断脚本 |
| `fix_content.py` | 一次性修复脚本 |
| `fix_wifi_sta.py` | 一次性修复脚本 |
| `fix_wifi_sta2.py` | 一次性修复脚本 |
| `trae_card_reference.bin` | 临时参考数据 |
| `examples/` | Lua 示例应用（Lua 路线已废弃） |
| `components_disabled/passport_runtime/` | Lua 运行时（Lua 路线已废弃） |
| `components_disabled/passport_link/` | .pap 蓝牙安装协议（已废弃） |

## 二、移动

| 文件 | 原位置 | 新位置 |
|------|--------|--------|
| `MiSans-Regular.otf` | 根目录 | `assets/fonts/` |
| `ble_provisioning.html` | 根目录 | `docs/guides/` |
| `wifi_provisioning_guide.html` | 根目录 | `docs/guides/` |
| `build_sim/` | 根目录 | `sim/build/` |
| `sdkconfig.sim` | 根目录 | `sim/` |

## 三、重命名

| 原路径 | 新路径 | 说明 |
|--------|--------|------|
| `pets/` | `humans/` | 精灵源 PNG 目录，与服务引擎名一致 |
| `components/human_display/pet/` | `components/human_display/human/` | 生成数据目录 |
| `pet_manifest.h` | `human_manifest.h` | 清单文件 |
| 76 个 `pet_*.c/h` 帧文件 | `human_*.c/h` | 所有精灵帧数据文件 |
| `human_display.c` 内部 `pet_` → `human_` | — | 类型名、变量名、宏全部更新 |
| 项目名 `FoloToy-AI-Passport` | `o-platform` | CMakeLists.txt project() |
| 日志 TAG `passport_main` | `o-platform` | main.c |
| 成品固件 `FoloToy-AI-Passport.bin` | `o-platform.bin` | 由项目名派生 |

## 四、当前结构

```
o-platform/
│
├── .github/                  # CI/CD 工作流
├── assets/                   # 资产
│   └── fonts/                #   字体源文件
├── build/                    # 真机构建产物（gitignored）
│   └── o-platform.bin        ← 成品固件
├── components/               # 自定义组件
│   ├── bsp/                  #   板级支持包
│   ├── human_display/        #   主页动态精灵引擎
│   ├── passport_core/        #   系统核心
│   ├── passport_ui/          #   UI 组件库
│   └── passport_voice/       #   语音输入
├── docs/                     # 文档
│   ├── development/          #   开发规范
│   ├── guides/               #   用户指南
│   ├── hardware-design/      #   硬件设计
│   ├── platform/             #   平台架构
│   └── software-design/      #   软件设计
├── humans/                   # 精灵源 PNG
├── main/                     # 应用层代码
├── managed_components/       # ESP-IDF 托管组件
├── sim/                      # 模拟器（QEMU 无射频）
│   ├── build/                #   模拟器构建产物
│   └── sdkconfig.sim         #   模拟器配置
├── tools/                    # 工具脚本
│
├── .gitignore
├── AGENTS.md / .zh_CN.md     # AI 代理行为规范
├── CMakeLists.txt            # 根构建入口
├── dependencies.lock         # 组件依赖锁
├── LICENSE
├── partitions.csv            # Flash 分区表
└── sdkconfig / sdkconfig.defaults  # 真机配置
```

## 五、根目录散落文件说明

| 文件 | 说明 |
|------|------|
| `.gitignore` | Git 忽略规则 |
| `AGENTS.md` | AI 代理行为规范（英文） |
| `AGENTS.zh_CN.md` | AI 代理行为规范（中文） |
| `CMakeLists.txt` | ESP-IDF 根 CMake 构建入口 |
| `dependencies.lock` | ESP-IDF 组件依赖版本锁 |
| `LICENSE` | 开源许可证 |
| `partitions.csv` | Flash 分区表 |
| `sdkconfig` | 当前构建配置（本地生成，不提交） |
| `sdkconfig.defaults` | 默认构建配置（版本控制） |