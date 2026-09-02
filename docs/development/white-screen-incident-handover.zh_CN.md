# 白屏死机故障交接报告（交下一手排查）

> 2026-08-29 紧急交接。设备：FoloToy AI Passport（ESP32-C3, 8MB, LVGL 9.5）。
> 仓库：`E:\code\ai passport\o-platform`。**注意：该仓库没有任何 git 提交，接手第一件事建议 `git init` + 全量提交基线。**

## 一、现状一句话

固件可编译可烧录（0 警告），但开机即白屏死机：main 任务持 LVGL 锁在 `lv_obj_get_display` 的显示链表遍历里死循环，LVGL 渲染任务饿死。**已排除宠物引擎；最大嫌疑是本机 main.c 曾被意外截断后由我手工重建的 ~400 行代码，或重建代码触发的 LVGL 9.5 渲染路径 bug。**

## 二、故障现象与复现

- 通电即复现：背光亮、白屏，无任何 UI；串口每 5 秒打印看门狗转储，任务 `main` 占满 CPU，IDLE 饿死。
- 回溯（已开 `CONFIG_ESP_SYSTEM_USE_FRAME_POINTER=y`，三次完全一致）：

```
Backtrace: 0x42044cae 0x4200a292 0x4200af0a 0x42113fc4
addr2line:
  0x42044cae → lv_obj_get_display  (lv_obj_tree.c:307)
  0x4200a292 → show_home           (main/main.c:459, 即 lv_screen_load 行)
  0x4200af0a → app_main            (main/main.c:1124, bsp_lvgl_lock{ show_home() })
  0x42113fc4 → main_task
```

- 时序：启动 ~0.7s 走到 show_home 即卡死（之后不会有「系统就绪」日志）；看门狗 5s 周期触发。
- 死循环体：`lv_obj_get_display` 内 `LV_LL_READ(disp_head, d)`（lv_obj_tree.c:314 附近）遍历 `LV_GLOBAL_DEFAULT()->disp_ll` 显示链表。

## 三、已确认的事实（按证据强度排序）

1. **宠物引擎已排除**：`pet_mode` 强制 false（关闭 pet_display）后同样死循环 → 与 pet_display 组件无关。
2. **显示链表本体在 show_home 建对象阶段是完好的**：已在 show_home 末尾（lv_screen_load 之前）插入诊断遍历，实测 `disp=0x3fca044c next=0x0`，单节点，遍历正常完成 → 排除「建对象阶段链表已损坏」。
3. **堆水位健康**：show_home 前 heap=111576/largest=98304，后 heap=110756（仅 ~800B 正常对象开销）→ 非内存耗尽。
4. **`d->screen_cnt=13` 异常**：正常应为 1~3（默认屏+加载屏）。13 恰好等于 cowboy 宠物帧数（巧合存疑但值得追）。注意读该值用的私有头 `display/lv_display_private.h`。
5. **死循环点在 lv_screen_load 内部**：诊断遍历（用公开 API `lv_display_get_next`）能正常走完，紧随其后的 `lv_screen_load()` 进 `lv_obj_get_display` 就转不动 → 挂在显示对象查找/链表环节，而该环节读的数据结构在上一步还是好的 → **指向上一步到这一步之间发生的结构破坏，或 LVGL 9.5 该路径的可重入问题**（show_home 此刻持有 bsp_lvgl 锁，LVGL 任务被挡在外面，不存在并发）。

## 四、最大嫌疑：手工重建的 main.c ~400 行

本轮一次批量替换失误把 main.c 第 107~520 行（从 `show_apps` 前向声明到 `format_setting_value` 定义之间）整段误删。随后由我按会话记录**手工重建**并拼接回去，重建后 0 警告可运行，但白屏在本轮首次出现。

重建清单（接手者请逐行核对这些函数/变量）：

| 重建内容 | 位置（现 main.c） |
|---|---|
| 前向声明补齐（show_settings/show_device_info/show_themes/show_wifi/show_network/show_transfer_page/handle_transfer_key/destroy_transfer_page） | ~101 行后 |
| SETTINGS_ROWS / SETTINGS_NAMES 常量 | ~106 |
| s_home_screen / s_home_bar / s_home_content / s_home_timer / s_home_btn_apps / s_home_btn_settings / s_home_selected / s_avatar_buf / s_avatar_dsc / s_home_nickname / s_home_college / s_home_major / s_home_student_id | ~120 |
| update_home_status（薄封装 → passport_status_bar_update） | 168 |
| destroy_home（pet_display_stop + 状态栏删除 + 界面清理） | 175 |
| show_home 本体（状态栏组件调用/内容区/头像双模式/字段循环/底栏/按钮/选中态/定时器/lv_screen_load） | 193-447 |
| destroy_native_view（wifi 轮询清理 + transfer + list + page） | 450 |

核对方法：原始同源代码在 `E:\code\pixel\ai-passport\main\main.c`（FoloToy 原始版，无本项目的功能改动，但函数结构与命名习惯同源），可对照找重建遗漏/笔误。

本轮同批还改了（也在嫌疑清单内，优先级低于重建核对）：
- 设置页移除「传输」行（顺带修了插入传输行导致的值刷新/编辑 off-by-one——原 bug：刷新写到第 1 行而值行在 2-5、编辑映射 selected-1 错位）
- 「网络与连接」页加第 4 行「传输」（OK → 传输界面）
- 「应用」页改为内置应用注册表启动器（当前空表显示「暂无应用」）

## 五、当前代码树状态（重要）

- **main.c 处于「诊断代码插入」状态，可编译可烧录**（0 警告），但包含两块临时诊断（搜 `临时诊断` / `[diag]` 注释即可定位）：show_home 前后堆水位日志 + 显示链表遍历（上限 8 节点）。修复完故障后删除这两块。
- `pet_mode` 被硬编码为 false（搜 `诊断：临时强制关闭宠物`），恢复时改回 true。
- sdkconfig 新增：`CONFIG_LV_USE_IMAGE=y`、`CONFIG_ESP_SYSTEM_USE_FRAME_POINTER=y`（后者是诊断用，可保留）。
- 主页字段值数据在设备 FAT 里是干净的 UTF-8（上一轮已字节级核验），与本故障无关。

## 六、已排除项

- pet_display 引擎/宠物渲染（pet off 仍复现）
- 内存耗尽（堆水位正常）
- 显示链表在建对象阶段损坏（诊断遍历正常）
- 传输页/配网页轮询清理（boot 阶段未运行）

## 七、建议排查路径（按优先级）

1. **核对重建代码**：用第四节清单逐行对拍（对照 `E:\code\pixel\ai-passport\main\main.c` 同名结构）。重点看 show_home 的对象创建顺序、s_home_screen/parent 关系、以及是否有对象被挂到错误父节点。
2. **堆毒化定位越界写**：sdkconfig 开 `CONFIG_HEAP_POISONING_COMPREHENSIVE=y` + 周期调 `heap_caps_check_integrity_all(true)` 打日志——若 disp_ll 节点被越界写坏，能定位到破坏者。
3. **LVGL 9.5 渲染路径嫌疑**：头像容器 `clip_corner=true` + 子对象 lv_image（pet 128 超出 102 容器被裁）+ 状态栏 lv_image——若怀疑渲染任务写坏显示结构，可临时把宠物/头像盒 clip_corner 关掉、pet 移出头像区做对比（当前 pet_mode=false 已等效关掉宠物渲染，若仍白屏则与渲染无关——已确认无关）。
4. **GDB 现场抓**：USB-Serial-JTAG 支持 OpenOCD JTAG 调试，设备死循环时 attach 上去看真实调用栈与对象树。

## 八、环境与工具速查

```powershell
# 构建（必须 PowerShell + 显式 IDF_PYTHON_ENV_PATH，git-bash 直跑 idf.py 会报 MSys 错误）
$env:IDF_PYTHON_ENV_PATH = "C:\Users\20372\.espressif\python_env\idf5.5_py3.12_env"
cd "E:\code\code tools\esp-idf-v5.5.3"; .\export.ps1
cd "E:\code\ai passport\o-platform"; idf.py build

# 刷机（约 10 秒）
powershell -ExecutionPolicy Bypass -File tools\flash.ps1

# 串口抓日志（COM6, 115200；RTS 脉冲触发复位后读 22 秒）
# 见会话中的 /tmp/serial_reader.py 模式：dtr=False; rts=True; sleep(0.1); rts=False; 循环读

# 符号化看门狗回溯（帧指针已开）
riscv32-esp-elf-addr2line -pfiaC -e build\FoloToy-AI-Passport.elf <地址...>

# 设备 IP: 192.168.1.71（HTTP /ping /fields /upload 可用，服务需在 设备:设置→传输→OK 手动开启）
```

## 九、本轮功能改动状态（与故障无关但一并交付）

- 「应用」页 = 内置应用注册表启动器（`BUILTIN_APPS` 表，当前空表显示「暂无应用」；以后移植功能加 `{ "名称", 打开回调 }` 一行即上列表，OK 打开）
- 「传输」并入「网络与连接」页第 4 行；设置页「传输」条目已移除
- **顺带修复**：传输行此前插入导致的设置页 off-by-one（值刷新写错行、编辑错位）——现已全部对齐
- 诊断代码定位：`[diag]` / `临时诊断` / `诊断：临时强制关闭宠物` 三处注释
