# ESP32-C3 固件构建指南（唯一权威）

> 本文件是 **E:\code\ai passport\o-platform** 项目构建的**唯一权威参考**。
> **构建/刷机失败时，先读本文件逐条排查，再动代码。**
> 所有内容均来自本机反复实锤的成功/失败经验，禁止凭旧印象臆断覆盖。

- 适用工程：`o-platform`（ESP32-C3，ESP-IDF v5.5.3，LVGL）
- 芯片：ESP32-C3（无 PSRAM）｜ 开发板 USB 串口：**COM6**
- 工具链根：`E:\esp\.espressif`（5.2 GB）

---

## 0. 一句话速查

| 场景 | 动作 |
|---|---|
| 日常增量构建 | 见 §1 标准流程 |
| 刚动过 sdkconfig / 分区 | 见 §2 全新构建（先删 build） |
| 配网/蓝牙/内存相关改动 | 构建前先看 §3 内存红线 |
| 构建报 `Permission denied` | 见 §4 坑 A |
| bootloader 子构建失败 | 见 §4 坑 B |
| 构建报 safe-delete 确认 | 见 §4 坑 C |
| 链接报 `undefined reference` | 见 §4 坑 D |
| 想看构建是否真的在跑 | 见 §5 判活 |
| 刷机 | 见 §6 |

---

## 1. 标准增量构建流程（已验证）

> ⚠️ **必须用 PowerShell，绝不能用 git bash / MSys / Mingw** —— `export.sh`/`export.ps1`
> 检测到 MSys/Mingw 直接报错退出。

```powershell
# ① 工具链路径（C 盘 C:\Users\20372\.espressif 是迁移前旧副本，已删，别再指向它！）
$env:IDF_TOOLS_PATH       = "E:\esp\.espressif"
$env:IDF_PYTHON_ENV_PATH  = "E:\esp\.espressif\python_env\idf5.5_py3.12_env"

# ② 若没设过下列变量，首次先补齐（避免 export 找错位置）
$env:ESP_ROM_ELF_DIR      = "E:\esp\.espressif\tools\esp-rom-elfs\20241011\"

# ③ 加载 IDF 环境（进入项目目录前，先在 esp-idf 目录执行）
cd "E:\code\code tools\esp-idf-v5.5.3"
.\export.ps1

# ④ 进工程构建
cd "E:\code\ai passport\o-platform"
idf.py build
```

**注意事项**
- **不要设置 `$env:TEMP / $env:TMP` 指向 E 盘**（如 `E:\tmp` / `E:\bldtmp`）——见坑 A。
  用系统默认 C 盘 TEMP 一次通过（C 盘当前有 13G，够用）。
- 产物：`build/o-platform.bin`。构建成功会打印 `Project build complete` 和分区使用率（如 57% free）。
- 构建慢排查：先看 §5 判活，确认编译真的在跑再等，别盲目杀进程。

---

## 2. 全新构建流程（动了 sdkconfig / 分区 / 大改后）

场景：还原固件、改 `partitions.csv`、`sdkconfig`/`sdkconfig.defaults` 大改、怀疑 build 目录与源码不一致。

```powershell
# 先彻底删 build 和 sdkconfig（否则 bootloader 子构建会触发 safe-delete 确认而卡死，见坑 C）
Remove-Item -Recurse -Force build
Remove-Item -Force sdkconfig

# 再按 §1 加载环境，然后：
idf.py set-target esp32c3
idf.py build
```

**为什么必须删**：build 目录里残留旧时代构建产物 + 当前源码混在一起时，bootloader 子构建
configure 阶段会检测到成百上千个 stale 临时文件，超过阈值 50 触发 `[safe-delete]` 二次确认，
非交互环境无法确认 → 构建中止。彻底删除后全新 configure 即可绕开。

---

## 3. ESP32-C3 内存红线（改动前必读）

**C3 无 PSRAM，所有内存都是片内 SRAM，堆极其紧张。** 以下改动都会直接吃堆，动前先算账：

| 项目 | 占用 |
|---|---|
| NimBLE 栈常驻 | 堆余仅 ~5KB（不开时余量更大） |
| opus 编解码栈（`xiaozhi_audio.c`） | **16384**（真实 HWM 仅 4880，勿再砍到 8K 会爆栈） |
| `xz_send` 栈 | 6144（3K 不够会 WS 发送爆栈崩溃） |
| 热点配网任务栈 | 8192（4K 会在 BLE 常驻时分配失败→"内存不足"） |
| 配网 StartAccessPoint 有 `ota_url[256]` 大局部变量 | — |

**铁律**
1. **配网前先停 BLE**：`wifi_prov_task` 里 `if (ble_prov_is_running()) ble_stack_stop();`。
   BLE 常驻时堆余 ~5KB，APSTA 分配 beacon 缓冲会 panic（`ieee80211_hostap_attach` Load access fault）。
2. **`CONFIG_BT_CTRL_BLE_MASTER` 永远别关**：= 控制器连接功能。关了 controller 拒绝可连接广播
   ADV_IND → `ble_gap_adv_start` 报 530。语音通道（0xA2B0）是 GATT 连接，必需。
3. **配网页任务栈 ≥ 8192**，否则 `xTaskCreate` 失败显示"内存不足"（会被 poll 覆盖成"配网失败"）。
4. 音频 init 时需求 ~33KB（16K opus 栈 + 17K 编码器，解码器延迟到首包），进小智页实测堆 69KB。
5. 新增大功能前：先 `esp_get_free_heap_size()` + 算连续块，BLE 常驻 + 大功能并发 = panic 温床。

---

## 4. 构建失败排查手册（按优先级）

### 坑 A：`Permission denied`（gcc `cc*.s` / `ranlib .a`）
**现象**：`gcc: error: ... Permission denied`（`.s` 汇编临时文件）或 `ranlib: unable to copy libxxx.a`。
**两个独立根因，需区分**：

**A1. TEMP 重定向到 E 盘（本机实锤）**
- 症状：gcc/ranlib 写 `E:\tmp` 的临时 `.s` / 复制 `.a` 失败。
- 根因：`$env:TEMP=$env:TMP="E:\tmp"` 与工具链写入机制不兼容/权限异常。
- 解法：**不设 TEMP，用系统默认 C 盘 TEMP** 重编一次即过。C 盘不够时才考虑临时重定向。
- 若已残留：先清理 `E:\tmp` 里的旧 `cc*.s`（每轮会累积几百个）。

**A2. 杀毒软件实时扫描锁 .a（本机实锤：Windows Defender + 火绒 HIPS）**
- 症状：`ar`/`ranlib` 归档新 `.a` 时偶发 `Permission denied`，或 ninja
  `remove(...obj.d): Access is denied`，**每次失败的是不同文件**（esp_https_ota→sdmmc→
  passport_core→bootloader 换着来）。
- 根因：实时防护扫描瞬间锁文件。**2026-09-01 实锤本机是 Windows Defender**（`Get-MpPreference`
  显示 `DisableRealtimeMonitoring=False`，且默认排除项被破坏为只剩 `C:\Program`）。
- **根治解法（本机实锤有效）：给工具链/框架/构建目录加 Defender 排除**（PowerShell 管理员）：
  ```powershell
  Add-MpPreference -ExclusionPath "E:\esp\.espressif"
  Add-MpPreference -ExclusionPath "E:\code\code tools\esp-idf-v5.5.3"
  Add-MpPreference -ExclusionPath "E:\code\ai passport\o-platform\build"
  Add-MpPreference -ExclusionProcess "riscv32-esp-elf-gcc.exe"
  Add-MpPreference -ExclusionProcess "riscv32-esp-elf-ar.exe"
  Add-MpPreference -ExclusionProcess "riscv32-esp-elf-ranlib.exe"
  Add-MpPreference -ExclusionProcess "ninja.exe"
  ```
- 若仍偶发（火绒等第三方 HIPS）：重试即过，可加 `IDF_CCACHE_ENABLE=0` 减少文件操作。
- ⚠️ **Defender 排除加完仍反复 `ar Permission denied`？→ 是 WSearch（Windows 搜索索引）**
  （2026-09-01 实锤：排除 Defender 后仍失败，排查发现 `WSearch` 服务在索引 build 目录、
  短暂锁新写入文件）。根治：PowerShell 管理员临时停服务再编译，编完重启：
  ```powershell
  Stop-Service WSearch -Force
  # ... 构建 ...
  Start-Service WSearch
  ```
- 判别：失败文件每次不同 → A2 扫描锁，加排除或重试；失败集中在 E:\tmp 且是 .s → A1，清 E:\tmp + 换默认 TEMP。

### 坑 B：bootloader 子构建失败（WorkBuddy 锁死 PATH）
**现象**：
```
CMake Error: CMake was unable to find a build program corresponding to "Ninja".
CMAKE_MAKE_PROGRAM is not set.
```
或 `riscv32-esp-elf-gcc is not a full path and was not found in the PATH`。
**根因**：bootloader 是 ExternalProject 子构建，经 `cmd.exe /C "cd /D ... && cmake.exe ..."`
调起 cmake，**完全依赖 PATH** 找 ninja/gcc。WorkBuddy 的 PowerShell 工具 spawn 子进程时用旧
PATH 快照重建环境，**丢弃 export.ps1 的会话级 PATH**；改注册表/`$env:PATH` 也影响不到 `cmd.exe /C`
子进程（宿主环境快照不刷新）。
**根治解法：用 bash 直接跑 bootloader 的 cmake configure + ninja，绕开 cmd.exe**：
```bash
export PATH="/e/esp/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20251107/riscv32-esp-elf/bin:\
/e/esp/.espressif/tools/ninja/1.12.1:/e/esp/.espressif/tools/cmake/3.30.2/bin:\
/e/esp/.espressif/tools/ccache/4.12.1/ccache-4.12.1-windows-x86_64:\
/e/esp/.espressif/python_env/idf5.5_py3.12_env/Scripts:$PATH"
export ESP_ROM_ELF_DIR="E:/esp/.espressif/tools/esp-rom-elfs/20241011/"
cd "/e/code/ai passport/o-platform/build/bootloader"
# configure 参数从 build.ninja 里 bootloader-configure 的 COMMAND 抄，末尾加 -DCMAKE_MAKE_PROGRAM
cmake.exe -DSDKCONFIG=... -DIDF_PATH=... -DIDF_TARGET=esp32c3 ... -G Ninja \
  -S "E:/code/code tools/esp-idf-v5.5.3/components/bootloader/subproject" \
  -B "E:/code/ai passport/o-platform/build/bootloader" \
  -DCMAKE_MAKE_PROGRAM="E:/esp/.espressif/tools/ninja/1.12.1/ninja.exe"
ninja   # 110/110 通过，生成 bootloader.bin
```
bootloader 单独构建好（stamp 存在）后，`idf.py -B build build` 会跳过 bootloader-configure，主工程完整通过。
> 注意：工具链版本路径需以 `E:\esp\.espressif\tools\` 下实际目录为准（esp-14.2.0_20251107 / ninja 1.12.1 / cmake 3.30.2 为本机实测）。

### 坑 C：`[safe-delete][SAFE_DELETE_BULK_CONFIRM_REQUIRED]`
**现象**：bootloader 子构建 configure 报 `{"count":2703,"threshold":50,...}`，卡住退出。
**根因**：build 目录残留大量 stale 临时文件超阈值 50，需删除确认，非交互无法确认。
**解法**：彻底删 `build` + `sdkconfig`，`idf.py set-target esp32c3` + `idf.py build`（见 §2）。

### 坑 D：链接报 `undefined reference to ...`（还原固件后）
**现象**：如 `undefined reference to ble_gattc_exchange_mtu`。
**根因**：还原固件后只恢复 `sdkconfig.defaults` 就删 `sdkconfig` 重 set-target，导致 Kconfig
裁剪了实际用到的功能。例：`CONFIG_BT_NIMBLE_GATT_CLIENT` **depends on**
`CONFIG_BT_NIMBLE_ROLE_CENTRAL`；备份 defaults 里 `CENTRAL=n` + `GATT_CLIENT=y` 自相矛盾，
GATT_CLIENT 被丢弃 → MTU 函数未定义。
**解法**：`sdkconfig.defaults` 把 `CONFIG_BT_NIMBLE_ROLE_CENTRAL=n` 改 `y`（voice_ble.c 用
`ble_gattc_exchange_mtu` 升 MTU 23→185，Mac central 不主动交换）。
**教训**：还原固件时，关键符号缺失 → 去对比备份的**实际 sdkconfig**
（`backup-*/source/o-platform/sdkconfig`）而非只看 defaults。defaults 只是目标默认，实际编译配置可能被构建覆盖。

---

## 5. 判断编译是否真的在跑（别盲目等/杀）

ESP-IDF v5.x 编译产物扩展名是 **`.obj` 不是 `.o`**（旧认知"0 个 .o = 没开始编译"是错的）。
```bash
# 看最新产物时间戳（bootloader 在 build/bootloader/ 下）
find build -name "*.obj" -mmin -1 | head
# 看是否有 gcc 编译器进程
tasklist | grep -i cc1        # Windows
# 看残留的 python/ninja/cmake 进程（构建慢/卡死排查）
tasklist | grep -iE "python|ninja|cmake"
```

---

## 6. 刷机（COM6）

```powershell
cd "E:\code\ai passport\o-platform"
idf.py -p COM6 flash monitor     # 刷入 + 串口日志
# 或仅刷：
idf.py -p COM6 flash
```
- 开发板 USB 串行设备 = **COM6**（已用 pnputil 确认）。
- 刷机后看开机日志验证功能（参考 §7）。

---

## 7. 开机日志验证要点（真机）

刷机后串口日志应依次出现（以热点配网 + BLE 语音时代为准）：
- `Passport Platform v1 启动` → 外设全就绪（屏/按键/电量/音频/语音）
- WiFi 开机自启连上已保存网络（如 `wifi:自动连接 CMCC-YFMLT`），拿 IP，`SNTP 校时`
- **开机不广播 BLE**（无"BLE 栈启动"，语音通道未注册）—— 当前设计 BLE 仅进语音页启用
- 进语音页 → 应见 `BLE 栈启动` + 手机 nRF Connect 能看到 `AI Passport` 广播
- 离开语音页 → BLE 停栈不再广播
- 进配网页 → 热点开启；离开即关

---

## 8. 目录约定

- 开发文档统一放 `o-platform/docs/development/`（本文件即在此）。
- 快照备份统一放 `o-platform/backup/`（**git 已忽略，不入库**，含大 tar.gz）。
- 项目长期记忆：`E:\code\ai passport\.workbuddy\memory\`。

---

## 附：历史构建血泪教训（防再犯速记）

1. **不要 git bash 编译** —— export 拒 MSys/Mingw。
2. **不要 TEMP 指向 E 盘** —— 见坑 A1。
3. **动了分区/大改先删 build+sdkconfig 全新构建** —— 见坑 C。
4. **bootloader 失败绕 cmd.exe，用 bash 直接 cmake+ninja** —— 见坑 B。
5. **还原固件看实际 sdkconfig 而非 defaults** —— 见坑 D。
6. **C3 无 PSRAM，先算堆再开大功能，配网前先停 BLE** —— 见 §3。
7. **ESP-IDF 产物是 .obj，判活用 find + tasklist** —— 见 §5。
