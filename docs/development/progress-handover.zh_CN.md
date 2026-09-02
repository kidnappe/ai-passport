# 开发进度交接文档

## 一、当前状态总览

| 项目 | 状态 |
|------|------|
| 固件版本 | FoloToy-AI-Passport v1 |
| 目标芯片 | ESP32-C3, 8MB Flash |
| ESP-IDF | v5.5.3 |
| 固件大小 | 1,789,280 bytes (0x1B4D60) |
| 剩余空间 | 43% (~1.29 MB) |
| 构建 | ✅ PASS（0 warning） |
| 刷入 | ✅ PASS |

## 二、已完成的功能

### 2.1 电池电量显示
- **文件**: `components/bsp/src/bsp_battery.c`
- CW2017 的 SOC 寄存器因电池 profile 不匹配返回无效值，改用电压估算
- 分段线性映射：3.0V=0%, 3.3V=20%, 3.7V=60%, 4.0V=90%, 4.2V=100%
- 移除了充电检测相关代码（寄存器地址不确定，功能不稳定）

### 2.2 WiFi 自动重连
- **文件**: `main/wifi_sta.c`
- 修复了 `wifi_sta_save_creds()` 保存凭据时不移到末尾的 bug
- 现在每次连接成功时：删除旧条目 → 压缩后续条目 → 添加到末尾
- `WIFI_EVENT_STA_START` 事件处理器里设配置 + 调 `esp_wifi_connect()`
- 断线后最多重试 3 次，连上后归零，`s_suspended` 标志位阻止配网期间重连
- 已测试通过：关机重启自动连接成功

### 2.3 传输页面（电子工牌配置）
- **网页**: `tools/transfer.html`（独立 HTML，在电脑浏览器打开）
- **设备 API**: `main/transfer_page.c`
- **功能**:
  - 四个字段：昵称、学院、专业、学号
  - 每个字段可设置：内容、字号、颜色、粗体
  - 头像上传（RGB565 148x220 .raw 格式）
  - 带 240×320 模拟设备屏幕实时预览
  - 设备端 API：`GET /ping`（检测连接）、`POST /upload`（接收 multipart 表单）
- **字段存储**: `/passport/{字段名}.txt`、`{字段名}_sz.txt`、`{字段名}_color.txt`、`{字段名}_bold.txt`
- **头像存储**: `/passport/avatar.raw`

### 2.4 主页电子工牌
- **文件**: `main/main.c`（`show_home()` 函数）
- 左侧：148×220 头像区域
- 右侧从上到下：昵称（白色）、学院（灰色）、专业（灰色）、学号（深灰色）
- 支持从 `/passport/*.txt` 读取颜色和字号配置
- 状态栏时间已改为 24px 显示（2026-08-28 对调：主页时间 24px、子页面状态栏时间 14px）

### 2.5 字体
- **字体**: Mi Sans Regular（从 `C:\Windows\Fonts\MiSans-Regular.otf` 复制到项目根目录）
- **字库文件**: 
  - `components/passport_ui/src/passport_ui_font_zh_14.c`（14px, ~298 KB）
  - `components/passport_ui/src/passport_ui_font_zh_24.c`（24px, ~652 KB；2026-08-28 更新：转 2bpp+缩减字符集后源文件 541 KB、flash 占用 74 KB，见 2.11）
- **生成工具**: `tools/generate_ui_font.py`
- **API**: `passport_ui_font_size(int size)` 返回对应字号字库指针
- 字符集：GB2312 常用字（~2000 字）+ 源码中文字符（~3784 字）+ ASCII + 图标

### 2.6 导航中新增"传输"页面
- **文件**: `main/main.c`、`main/transfer_page.h`
- 在设置页面新增"传输"选项（在"网络与连接"之后）
- 定义了 `VIEW_TRANSFER` 视图，完整的按键处理和销毁逻辑

### 2.7 粗体显示（合成粗体）
- **文件**: `main/main.c`（`show_home()` 的字段循环）
- 读取 `/passport/{字段}_bold.txt`，为 `1` 时在该字段正本**下方**先画一个右移 1px 的同色同字号副本，形成加粗观感
- **为什么不生成 Bold 字库**：系统只有 `MiSans-Regular.otf`，没有 Bold 字重；且 flash 仅剩约 724 KB（裁剪插件路线前为 673 KB），而 14px 字库实测占 291 KB、24px 占 637 KB，再塞一套 Bold 必然爆分区（详见 4.3）
- 零字库开销，对任意字号、任意颜色都生效

### 2.8 头像自动转码
- **文件**: `tools/transfer.html`（仓库根目录，不是 `o-platform/tools/`）
- 网页端接受 PNG / JPG / WEBP，用 Canvas 居中裁剪（`cover`）缩放到 148×220，再转成 **小端 RGB565** 后上传
  - 字节序：LVGL 原生小端，`components/bsp/src/bsp_display_lvgl.c` 会再交换高低字节给 SPI 屏，因此 `.raw` 必须低字节在前
- 若选择的本来就是 `.raw`，会先校验长度必须为 148×220×2 = 65,120 字节，不符则报错拒绝，不再写入坏文件
- 设备预览区同步显示头像，且预览尺寸已按固件真实布局校正（外框 252×332 → 屏幕 240×320 = 状态栏 28 + 内容 220 + 底栏 72；头像 x=10/148×220；文本区 x=168/宽 62）
- **回归测试**: `node tools/check_transfer_rgb565.js`（仓库根目录），校验 RGB565 转换与字节序

### 2.9 传输页二次进入修复
- **文件**: `main/transfer_page.c`
- `transfer_stop()` 原先只清 `s_status` / `s_url_label`，没清 `s_page`，导致离开传输页后再次进入时 `show_transfer()` 在 `if (s_page) return;` 处提前返回，页面空白且按键无响应
- 现在 `transfer_stop()` 调用 `passport_ui_page_destroy(s_page)` 并把 `s_page` 置空

### 2.10 插件路线裁剪 +「应用」改名（2026-08-28）
- **背景**: 项目放弃「Lua 插件 + `.pap` 蓝牙安装」路线，改为固件内模块化集成社区玩法
- **移出构建**（源码保留在 `components_disabled/`，含恢复说明）:
  - `components/passport_runtime`（Lua 运行时，连带 `espressif__lua` 解释器）
  - `components/passport_link`（`.pap` 蓝牙安装协议；BLE 配网 `main/ble_prov.c` 自管 NimBLE，不受影响）
- **main.c 清理**: 删除 `VIEW_LUA_APP` 视图、`EVENT_LINK_FRAME`/`EVENT_PACKAGE_INSTALLED` 事件、`on_link_frame`/`on_package_install` 回调、`passport_link_init/deinit` 调用
- **蓝牙开关真实生效**（同日跟进）: BLE 现在唯一用途是 WiFi 配网，故「网络与连接」页蓝牙开关直接控制配网——关闭后 `show_wifi_prov()` 显示「蓝牙已关闭」并拒绝启动；配网进行中关闭开关会终止配网任务（`s_wifi_cancel`）；配网页「长按 OK 取消」现在也会真正终止任务（原先只回主页，任务继续在后台跑）。主页状态栏蓝牙图标跟随开关变色：白色=开启、暗灰 0x555555=关闭（沿用 `update_home_status()` 的既有约定，初始值由 `show_home()` 给定、每 5s 刷新）。顺带修复：离开配网页时轮询定时器未销毁，`wifi_poll_cb` 会写已释放的 label（悬空指针），现在 `destroy_native_view()` 统一销毁定时器并清空 label 指针
- **改名**: 主页按钮与相关页面文案「插件」→「应用」（主页按钮、应用管理/应用详情页、设备信息页），代码符号同步改名（`VIEW_APPS`、`show_apps()`、`s_home_btn_apps` 等）
- **保留不动**:
  - `appfs` 分区及 `passport_storage`：现作为 `/passport` FAT 数据分区存放工牌字段配置与头像
  - `passport_core` 的 `passport_app_registry`/`passport_package`：「应用管理」页保留扫描/卸载能力，可清理历史安装的 `.pap` 残留
- **收益**: 固件 2,456,592 → 2,404,416 bytes，**省 51 KB**；剩余空间 673 KB → **724 KB (24%)**。注意：这对字库空间问题帮助有限（见 4.3）

### 2.11 字库瘦身：24px 转 2bpp + 缩减字符集 + fallback（2026-08-28，同日第二轮）
- **两项优化同时落地**（此前 4.3 的两条待选路线）:
  - 24px 字库 bpp 4 → **2**（所有字符统一保持 24px，仅抗锯齿档位 16 → 4，边缘略硬）
  - 24px 字符集从全量 GB2312 一级（3755 字）缩减为 **ASCII + 图标 + 源码用字 + 高频 603 字/常见姓氏**（符号集 728 字）
  - 24px 字体结构体的 `fallback` 指向 14px 字库：**24px 库里没有的字自动用 14px 字形渲染**（同字段内大小混排），不再出现空白
- **实现**: `tools/generate_ui_font.py` 重构为双字库规格（14px 4bpp 全量 / 24px 2bpp 紧凑 + fallback 后处理注入），生成后自动插入 `LV_FONT_DECLARE` 与 `.fallback`，`--check` 会校验 bpp/字符集/fallback 三项
- **效果**: 24px 字库 675 KB → **74 KB**；两套字库合计 928 KB → 403 KB；固件 2,404,416 → **1,789,280 bytes**；剩余空间 724 KB → **1.29 MB (43%)**
- **验机要点**: ①时间栏（24px 数字）正常；②字段设 24px 时常用字大小正常；③填生僻字（如「䶮」「燚」）应以小一号 14px 渲染而非空白；④14px 字段观感与之前完全一致；⑤细笔画（钩、点）边缘略糙属 2bpp 预期

### 2.13 主页字段布局修复：动态堆叠 + 标签冒号 + 昵称 24px（2026-08-29）
- **重叠根因**: 右栏宽 62px，字段是固定 y 坐标（16/52/78/104），24px 文本折行后昵称高度膨胀压到学院；网页预览用 nowrap+ellipsis 单行截断，所以预览看不出
- **修复**: 四个字段改为纵向动态堆叠——每行渲染后 `lv_obj_update_layout` 实测高度再排下一行（间隔 8px），折行多少都不会重叠
- **标签**: 除昵称外的字段渲染为「学院：值 / 专业：值 / 学号：值」（全角冒号，标签来自固件 `field_labels`，不写入 `_txt` 文件，值仍是纯文本）
- **昵称**: 固定 24px（忽略 `_sz.txt`），网页端昵称字号下拉只保留 24 档
- **预览同步**: transfer.html 预览改为与固件一致的动态堆叠+折行渲染，初始文案与空值默认（昵称 o-Platform、其余「标签：」）对齐

### 2.14 昵称 48px 迷你字库 + 主页版式重排：单行不折行（2026-08-29）
- **迷你字库**: `passport_ui_font_zh_48`——48px 4bpp、仅含「周旋」二字（源文件 8.4KB，flash ~3KB），fallback 链 48→24→14；由 `generate_ui_font.py` 新增迷你规格生成（`sources` 支持按规格携带字体/码段，无 ASCII 与图标段）。若要换名字，改脚本里 `frozenset("周旋")` 与码段 `0x5468,0x65CB` 重新生成即可
- **单行不折行**: 主页字段一律 `LV_LABEL_LONG_DOT`（超宽截断显示省略号），文本列加宽到 **142px** 恰好容纳最长的「学院：马克思主义学院」（10 字 ×14px）
- **版式**: 头像 148×220 → **80×220**（img_x=6、间距 6），文本列 62 → 142px（info_x=92）；字段纵向动态堆叠（起始 y=24、行距 14），昵称 48px 单行 96px
- **头像尺寸变更**: 网页端 `AVATAR_W` 同步改 80（raw 80×220×2 = 35,200 字节），旧 148×220 的 avatar.raw 尺寸校验不过会显示占位，**需重新上传头像**
- **网页**: 昵称字号档改 48（唯一档）、预览布局/单行截断与固件一致；回归测试改为从 HTML 实际解析 AVATAR_W/H 再断言（原来 148 断言是恒真式）
- **验机要点**: 「周旋」48px 单行；「学院：马克思主义学院」「专业：思想政治教育」「学号：2510414038」各占一行不折行；换昵称里非「周旋」的字会以 24px 渲染（fallback 链）

### 2.15 主页版式按参考图重排（2026-08-29 第二轮）
- **背景**: 上一轮 142px 文本列 + DOT 截断把冒号后的值吃掉了（DOT 会预留省略号宽度，实际可见 ~128px），80px 头像也偏小、学号被推出视口
- **新几何**（参考 FoloToy 官方 UI）:
  - 头像 **88×220**，从左边缘 (0,28) 顶格铺到文本列左缘（88px 处），占满内容区高度
  - 文本列 **152px**（88→240），**右对齐、紧贴右边界**；152px 含 DOT 余量，「学院：马克思主义学院」完整显示
  - 纵向固定坐标: 昵称 y=8（与状态栏只留 8px）、字段 y=63/91/119（间隔 28），全部顶置
- **头像尺寸再次变更**: raw 为 88×220×2 = 38,720 字节，网页端已同步，**需重新上传头像**
- **实现简化**: 单行不折行后高度可预知，纵向坐标改为固定值，去掉了动态测高逻辑

### 2.16 multipart 解析 bug 修复：值被 "
" 污染（2026-08-29 第三轮，实机照片定位）
- **症状**: 实机照片显示「学院：/专业：」冒号后无值、昵称「周旋」压住专业/学号、头像一直显示占位
- **根因**（`transfer_page.c` 上传解析器）: 解析完 `Content-Disposition` 头后 `pos` 只跳过头的行尾，落在 part 头与值之间的**空行**（
）上，`in_field`/`in_file` 分支把空行一起写入文件 → 所有上传文件都带 "
" 前缀:
  - `nickname.txt` = "
周旋" → LVGL 渲染两个空行 → 昵称下移两行压住字段
  - 值文件的值被推到第二行，正好被 48px 昵称盖住 → 「冒号后无内容」假象
  - `avatar.raw` 多 2 字节 → 尺寸校验永远失败 → 头像从第一版起就没真正上传成功过
- **修复**: ①解析器跳过 part 头后的空行（根治，文本与头像同时修复）；②`save_field` 写值前剥离前导 
（保险）；③主页读取侧 trim 首尾空白（直接治愈设备上已污染的旧文本文件，无需重传值）
- **微调**: 昵称 y=12、字段 y=68/96/124（48px 字形略超出行高上沿，留安全边距）
- **注意**: 头像是二进制，旧污染文件无法自动治愈，**需重新上传一次头像**；文本值无需重传

### 2.17 multipart 分块边界 bug + 远程运维接口（2026-08-29 第四轮）
- **症状**: 值推送时「专业」丢失——正文超 1024 字节分块接收后，**恰好骑在块边界上的 part 头被整段丢弃**（解析循环遇半行直接 break 丢数据）
- **解析器重写**: 缓冲区带 `filled` 持久化——半行压回缓冲头与下一块拼接；值/文件流跨块时暂存 `field_value`/直接写文件并清空缓冲；`
` 空行跳过保留。单一大 POST（网页端 16 字段+头像）不再丢 part
- **新增**: `GET /fields` 诊断接口（返回四个字段的当前存储值，远程核查用）；文本列右边距 3px（修复 48px「旋」字形贴屏被切）
- **运维记录**: 设备 IP 经串口启动日志获取（192.168.1.71）；四个字段值已通过分请求 curl 直推落盘（绕过旧解析器分块 bug），`/fields` 核验全部正确
- **服务启停**: 按用户要求**保留手动启动**（设置→传输→按 OK），未做开机自启

### 2.20 httpd 线程刷死修复 + 排版对齐参考图（2026-08-29 第七轮）
- **httpd 假死**: 上传完成回调在 httpd 线程直接 `passport_ui_label_set_text`（无 LVGL 锁），与渲染任务并发刷死在 lv_refr 重绘链表（看门狗实锤：httpd 占满 CPU，PC 落在 lv_refr.c）。修复：该 UI 更新包 `bsp_lvgl_lock`。**教训：httpd 线程永远不许直接碰 LVGL**
- **副作用说明**: 该刷死导致上一轮的字段值推送半途失败（部分字段未落盘），本轮修复后重推
- **排版对齐参考图**: 昵称居中于文本列顶部（对应 FoloToy 标题），字段列内**左对齐**（对应 Token值/666 排版方向）；头像/列宽几何不变

### 2.21 主页顶/底栏参照设置页（2026-08-29 第八轮）
- 顶栏（状态栏）与底栏（按钮区）底色从纯黑改为**主题 surface**（默认 0x111111），并按设置页样式加**分割线**（顶栏下缘 / 底栏上缘，divider 色 0x222222）
- 中间内容区背景保持主题 background（0x000000）不变；底栏以独立容器实现，两个按钮叠于其上，坐标不变
- 网页预览 CSS 同步（status-bar/bottom-bar 底色与分割线）

### 2.23 头像区动态化：像素宠物引擎接入（2026-08-29 第十轮，方案源自 E:\code\pixel\动态帧技术说明.md）

**四条尺寸派生规则**（写进 `pet_display.c` 头注释，头像区大小调整的全联动承诺）：
1. 单一尺寸源：所有几何只从 `pet_display_cfg_t` 的 box_w/box_h 推导；
2. 地面线锚底：精灵 y = box_h − draw_h + 跳跃偏移，框变高变矮脚始终踩框底；
3. 自动整数倍缩放：box 短边 ÷ 64 向下取整（至少 1x），像素画不出现非整数缩放歪像素；
4. 走动边界派生：[2, box_w − draw_w − 2]，撞墙进入静止池。

**实现**:
- 新组件 `components/pet_display/`：引擎 `pet_display.c`（demo_pet.c 适配版：单实例、无建屏/按键/背景层）+ 裁剪版 `pet_manifest.h`（仅 chick：JUMP 4 帧/SLEEP 2 帧/WALK 3 帧）+ `pet/chick/` 9 帧资源（rgb565a8 64×64，flash ≈108KB）
- 生命周期：`show_home()` 按头像模式调 `pet_display_start(容器, box_w, box_h)`；`destroy_home()` 调 `pet_display_stop()`；引擎全部动作在 lv_timer（LVGL 任务内），线程安全
- **双模式**: `/passport/avatar_mode` 为 "photo" 时走静态照片路径，其余（含缺省）为动态宠物；网页开关随最终批量更新加入
- **sdkconfig**: 启用 `CONFIG_LV_USE_IMAGE=y`（原工程裁掉了 lv_image 控件）
- **资源账**: 固件 0x1D1760 = 1,906,528 B（lv_image 控件 + 9 帧 ≈ +117KB），剩余 **1.18MB (39%)**；RAM 增翻转缓冲 16KB（.bss 静态）
- **验机**: 回主页——头像区出现小鸡（走动→撞墙睡觉→转身反向，随机跳跃），字段列不受影响

### 2.24 LPC 角色接入：cowboy + 全动作 + 2x 放大（2026-08-29 第十二轮）
- **新工具 `tools/lpc2pet.py`** + **文件夹约定**: 源 PNG 每角色一个文件夹 `o-platform/pets/<名字>/`，生成物进 `components/pet_display/pet/<名字>/`，换角色 = 改 manifest 注册表 + CMake SRCS
- **角色 cowboy**（用户 LPC 导出表切片，特征命名：宽檐牛仔帽+橙衫蓝裤）: 行表取自生成器 whichAnim 下拉的 data-row 权威数据（walk=8、spellcast=0、thrust=4、slash=12、shoot=16）；**全动作联合包围盒裁剪 + 底边锚定**保证帧间对齐
- **动作 6 个（全原地）**: stand 1 帧 / slash 3 帧 / spellcast 3 帧 / thrust 3 帧 / shoot 3 帧 = 13 帧 @128×128 rgb565a8（≈565KB flash）
- **2x 放大**: LPC 64px 格内容 ~50×59 → 2x ≈ 100×118，占满 102×140 头像区（98% 宽/84% 高）。**代价**: 无行走空间（几何互斥），全部动作原地表演；walk 动作随之移除
- **引擎调整**: ①LPC 素材自带右朝向，删除翻转缓冲（省 65KB .bss）；②精灵宽于框时自动居中放置并禁用走动；③rest 池内轮换（move 池为空时动作到期随机切换其它静止动作，而非原样重播）
- **坑**: prep_pet 清单写在 `pet/pet_manifest.h`，组件根目录不能有同名旧清单；CMake SRCS 路径必须是 `pet/cowboy/`（漏层会报 cannot find source file）；prep_pet 的 glob 目录不允许无 motion 字段的杂图（_contact.png 之类）
- **资源账**: 固件 2,437,536 B，剩余 **694KB (23%)**。chick 素材保留在 `pet/chick/` 未编译，可随时切回

### 2.24 LPC 角色接入：切片器 + cowboy（2026-08-29 第十一轮）
- **新工具 `tools/lpc2pet.py`**: Universal LPC 角色生成器导出的整表 → 动态帧宠物素材切片器。行表取自生成器 whichAnim 下拉的 data-row/data-cycle 权威数据（walk=row 8/9 帧、idle=row 22/2 帧、jump=row 26/5 帧）；右朝向 = 4 行块的第 4 行；**全动作联合包围盒裁剪**（帧间对齐不破坏）后贴回 64×64 底边锚底画布
- **LPC 图层覆盖限制（实测发现）**: 衣服图层只覆盖核心动画（walk/spellcast/slash 等），idle/jump 等扩展动画只有裸身体图层——混用会「着装闪烁」。故 cowboy 只取 walk 5 帧 + walk 帧 0 复用为 1 帧 stand（rest 池），共 6 帧 ≈72KB
- **文件夹约定（用户要求）**: 源 PNG 每角色一个文件夹 `o-platform/pets/<名字>/`（本角色特征命名 **cowboy**：宽檐牛仔帽+橙衫蓝裤）；生成物进 `components/pet_display/pet/<名字>/`；复用/换角色 = 改 pet_manifest 注册表 + CMake SRCS
- **坑**: prep_pet 会把清单写到 `pet/pet_manifest.h`，组件根目录不能有同名旧清单（相对 include 优先命中旧文件）；glob 目录下不允许 `_contact.png` 之类无 motion 字段的杂图
- **状态**: cowboy 已注册为头像宠物并刷机（固件 0x1C86E0，剩余 1.21MB/41%）；chick 素材保留在 `pet/chick/` 未编译，复用随时可切回

### 2.22 头像下边界与学号行齐平（2026-08-29 第九轮）
- 头像高度 220 → **140**（= 最后一行字段 y 124 + 14px 行高 16），与学号字段下边界齐平；字段布局常量上移至头像之前
- 头像 raw 变更：102×140×2 = **28,560 字节**，网页/回归测试同步；**需重传头像照片**

### 2.18 编码事故修复 + 昵称降档 24px（2026-08-29 第五轮，flash dump 取证）
- **症状**: 实机照片——昵称「周旋」完全消失，学院/专业的值变豆腐块（缺字形占位框），仅纯数字的学号正常
- **取证**: `esptool read_flash` dump appfs 分区，字节级检查——`周旋` 存的是 **GBK**（D6 DC D0 FD）而非 UTF-8，三个文本值全中
- **根因**: 用 git-bash 里 curl 推送中文时，Windows 控制台按 GBK 编码传参，设备原样存了 GBK 字节；LVGL 按 UTF-8 解码全部落空（昵称的 48px 迷你字库只含 UTF-8 码点 → 整个消失；值 → 豆腐块；纯 ASCII 的学号免疫）。此前 `/fields` 显示「干净」是终端以 GBK 解码造成的假象——**教训：跨端推中文必须走文件或显式 UTF-8 的 HTTP 客户端，严禁控制台直传**
- **修复**: 值改用 UTF-8 文件 + `curl -F "field=<file"` 方式重推；昵称按用户要求降一档至 **24px**
- **字库变更**: 移除 48px 迷你字库（规格/CMake/声明/分支全撤，「周旋」二字收入 24px 紧凑字符集，834 → 836 字形）；网页昵称档位回 24
- **验机**: 回主页——「周旋」24px 加粗在顶，「学院：马克思主义学院」「专业：思想政治教育」「学号：2510414038」各一行右对齐；头像占位待用户重传

### 2.19 半角冒号 + 头像扩展至文本列左缘（2026-08-29 第六轮）
- 字段分隔冒号从全角「：」改半角「:」（ASCII 段字形现成，每行省 ~7px）
- 最长行「学院:马克思主义学院」≈133px → 文本列 135px、右留 3px
- 头像 88 → **102×220**，右缘直接顶住文本列左缘（info_x=102，零空隙）
- 头像 raw 变更：102×220×2 = 44,880 字节，网页/回归测试已同步，**需重传头像**
- 待办：设备上的值仍是 GBK 污染字节（上轮 curl 事故），需在服务启动后用 UTF-8 文件方式重推（push/ 目录已备好）

### 2.12 状态栏统一为一套系统（2026-08-28，同日第三轮）
- **背景**: 主页状态栏（`main.c` 手写）与子页面状态栏（`passport_ui_page_create` 组件）长期并存，样式漂移——「状态栏时间 24px」只改过组件侧、主页漏改即为例证
- **实现**: 新增 `passport_status_bar` 共用组件（`passport_ui.h` 声明、`passport_ui.c` 实现），两种模式:
  - `PASSPORT_STATUS_BAR_HOME`：黑底、24px 时间、星期（跟随时间标签右缘）、白/灰图标
  - `PASSPORT_STATUS_BAR_PAGE`：主题 surface 底 + 分隔线、左侧标题、14px 时间、主题色图标
  - 刷新逻辑唯一化: 时间、星期、电量六档配色、蓝牙/WiFi 图标灰白联动（读 `s_bt_enabled` / `s_wifi_enabled` / `wifi_sta_is_connected`）
- **接入**: 主页 `show_home()` 55 行手搓状态栏 → 1 行 `passport_status_bar_create(..., HOME, NULL)`，`update_home_status` 变薄封装，顺带消灭了「按下标取 child 2/3 刷图标颜色」的脆弱写法；子页面 `passport_page_t` 删掉 7 个状态栏字段，全部走共用结构体
- **保留**: 上一轮对调（主页时间 24px、子页面时间 14px）原样保留，现由 `mode` 参数表达

## 三、修改的文件清单

### 新增文件
| 文件 | 说明 |
|------|------|
| `main/transfer_page.h` | 传输页面头文件 |
| `main/transfer_page.c` | 传输页面实现（HTTP 服务器 + API） |
| `tools/transfer.html` | 独立传输网页（电脑浏览器打开） |
| `components/passport_ui/src/passport_ui_font_zh_24.c` | 24px 字库（生成文件） |
| `MiSans-Regular.otf` | Mi Sans 字体源文件（项目根目录） |
| `tools/check_transfer_rgb565.js` | 网页端 RGB565 转换回归测试（仓库根目录 `tools/`） |
| `docs/development/progress-handover.zh_CN.md` | 本文档 |

### 修改文件
| 文件 | 修改内容 |
|------|---------|
| `main/main.c` | 添加传输页面、主页四字段显示、VIEW_TRANSFER 视图；字段渲染改为「先读全部配置再建 label」，加入合成粗体；移除未使用的 `passport_ui_font_zh_14` 声明（消除编译告警）；（2026-08-28）裁剪插件链路（passport_link/runtime/Lua）、「插件」改名「应用」；配网取消/BLE 启动失败后按 `s_wifi_enabled` 重新拉起 WiFi（原先 radio 停了就没人恢复）；主页状态栏时间改 24px、`passport_ui` 页面状态栏时间改 14px（两处样式对调，主页星期改用 align_to 跟随时间标签）；（同日）主页与子页面状态栏统一为 `passport_status_bar` 共用组件；（2026-08-29）主页字段动态堆叠修重叠、除昵称外加「标签：」前缀；（同日）新增 48px 迷你字库（仅「周旋」）；（第二轮）头像 88×220 左缘顶格、文本列 152px 右对齐贴右边界、字段固定纵坐标全顶置 |
| `main/CMakeLists.txt` | 添加 `transfer_page.c`、`esp_http_server` 依赖；（2026-08-28）移除 `passport_link`、`passport_runtime` 依赖 |
| `main/wifi_sta.c` | 修复凭据保存 bug、添加重连逻辑、断线原因日志；（2026-08-28）`wifi_sta_stop()` 挂起自动重连、`wifi_sta_init()` 恢复，消除手动停止时 disconnect 事件触发 `esp_wifi_connect()` 与 stop/deinit 的竞争 |
| `main/transfer_page.c` | `transfer_stop()` 销毁并清空 `s_page`，修复二次进入空白 |
| `components/passport_ui/CMakeLists.txt` | 添加 `passport_ui_font_zh_24.c` |
| `components/passport_ui/src/passport_ui.c` | 添加 24px 字库声明、`passport_ui_font_size()` 函数、状态栏时间 24px |
| `components/passport_ui/include/passport_ui.h` | 添加 `passport_ui_font_size()` 声明 |
| `tools/generate_ui_font.py` | 字体源改为 Mi Sans、添加 `common_glyphs()` 缩减字符集；（2026-08-28）重构为双字库规格：14px 4bpp 全量 / 24px 2bpp 紧凑字符集 + `fallback` 自动注入与校验 |
| `tools/transfer.html` | 头像支持任意图片并转 RGB565；字号选项收敛为 14/24；预览按固件真实尺寸校正；上传统一带 `_sz`/`_color`/`_bold` |
| `tools/check_transfer_rgb565.js` | 新增：RGB565 转换与字节序回归测试 |

### 移动文件（2026-08-28）
| 文件 | 说明 |
|------|------|
| `components_disabled/passport_runtime` | 原 `components/passport_runtime`，Lua 运行时，移出构建 |
| `components_disabled/passport_link` | 原 `components/passport_link`，`.pap` 蓝牙安装协议，移出构建 |
| `components_disabled/README.md` | 移除原因与恢复方法 |

> 路径说明：`tools/transfer.html` 与 `tools/check_transfer_rgb565.js` 位于**仓库根目录**的 `tools/`，不在 `o-platform/tools/`。

## 四、已知问题 / 待办

### 4.1 蓝牙传输未实现
当前传输页面仅支持 WiFi HTTP 方式。~~方案一：用现有 Passport Link 打包机制，将字段打包成 `.pap` 包传输~~ **已随插件路线裁剪废弃（见 2.10，passport_link 已移出构建）**。剩余可选：
- **方案二**：新增 GATT 特征（`FIELD_DATA`、`AVATAR_CTRL`、`AVATAR_DATA`），走手机 App 或网页 Bluetooth API

### 4.2 粗体未生效 — ✅ 已解决
已用**合成粗体**实现（见 2.7）。不再需要下载 Mi Sans Bold：系统里没有该字重，且即便有，一套 Bold 字库也塞不进剩余的约 724 KB。

### 4.3 字库空间偏大 — ✅ 已解决（见 2.11，两条路线都已落地）
从 `build/FoloToy-AI-Passport.map` 读出的真实占用：

| 字库 | 实际占 flash | 说明 |
|------|-------------|------|
| `passport_ui_font_zh_14` | 291 KB (0x48B62) | 4bpp + 压缩 |
| `passport_ui_font_zh_24` | 637 KB (0x9F3F5) | 4bpp + 压缩 |
| 合计 | **928 KB** | 应用分区 3 MB，固件共占 76%（2026-08-28 裁剪插件后剩 ~724 KB） |

> ⚠️ 以下为 2026-08-28 早间的决策记录；当天已按 2.11 落地两条优化路线，正文保留当时推理，数字为旧固件的。

**不建议再缩减字符集**，原因：`nickname` / `college` / `major` 是用户在网页上随便填的，字库一旦砍到「源码用字 + 常用 500 字」，生僻人名、专业名就会渲染成空白。当前 `common_glyphs()` 已经只取 GB2312 的 0xB0–0xC7 行（约 2256 字），再往下砍收益/风险比不划算。

若确实要腾空间，优先级更高的方向是：
- 把 24px 字库的 bpp 从 4 降到 2（字形边缘会变硬，预计省 30%+）
- 或让 24px 只覆盖 ASCII + 常用 500 字，中文超集回退到 14px（LVGL `fallback` 机制）

### 4.4 头像格式限制 — ✅ 已解决
网页端已支持任意 PNG/JPG/WEBP 自动转码（见 2.8），无需用户手工转换，也不需要占用 flash 的解码库。

### 4.5 字号选项与实际能力不一致 — ✅ 已解决
`passport_ui_font_size()` 只有 14 / 24 两档，但网页原先提供 12~24 共六档，选 16/18/20/22 会**静默回退到 14px**。已把下拉收敛为 14 / 24，界面与真实能力一致。若将来要支持中间档，空间已不再是障碍（剩余约 1.29 MB）；新增档位建议沿用 2.11 的「缩减字符集 + 2bpp + fallback」配方（一档 24px 级别仅 ~74 KB），避免再生成全量字符集字库。

## 五、构建与刷写

```bash
# 激活 ESP-IDF 环境
cd E:\code\code tools\esp-idf-v5.5.3
.\export.ps1

# 构建
cd E:\code\ai passport\o-platform
idf.py build

# 查看日志
idf.py monitor
```

**刷写推荐走快速通道**（实测 19 秒 vs `idf.py flash` 约两分钟）：

```powershell
# 前提：先跑过 idf.py build，build/ 里有现成产物
powershell -ExecutionPolicy Bypass -File toolslash.ps1
```

`tools/flash.ps1` 跳过 `export.ps1` 环境激活（约 1 分钟）和 `idf.py` 的构建检查，直接用 esptool 烧 `build/` 里的镜像。**它只烧不编**——改了代码务必先 `idf.py build` 再用它。剩余 19 秒里约 12 秒是 2.4MB 镜像的片上 flash 编程时间（瓶颈在芯片写入速率，不是串口波特率），已经接近物理下限。

## 六、使用流程

1. 设备开机 → 自动连接 WiFi（首次需配网；注意「网络与连接」页的蓝牙开关需为「开」，否则配网页会提示蓝牙已关闭）
2. 设置 → 传输 → 按 OK 启动传输服务（屏幕会显示设备 IP）
3. 电脑打开**仓库根目录**的 `tools/transfer.html`，填入设备 IP，点「检测」确认连通
4. 填写昵称/学院/专业/学号，调颜色（14/24 两档字号）与粗体
5. 头像直接选一张 PNG/JPG 即可，网页自动转成 148×220 RGB565；也可直接选已有的 `.raw`
6. 点击「传输到设备」
7. 返回主页即可看到新工牌（无需重启，`show_home()` 每次都会重读 `/passport/*.txt` 与 `avatar.raw`）

## 七、开发提示

- **不要用 git bash 跑 `idf.py`**：`export.sh` 会检测到 MSys/Mingw 并直接报错退出。用 PowerShell：

  ```powershell
  $env:IDF_PYTHON_ENV_PATH = "C:\Users\20372\.espressif\python_env\idf5.5_py3.12_env"
  cd "E:\code\code tools\esp-idf-v5.5.3"; .\export.ps1
  cd "E:\code\ai passport\o-platform"; idf.py build
  ```

  必须显式指定 `IDF_PYTHON_ENV_PATH`，否则 export 会去找 3.13 的虚拟环境（`idf5.5_py3.13_env`）并报「not found」。
- **从 Git Bash 调 PowerShell 也会踩同一个坑**：子进程会继承 `MSYSTEM` 环境变量，`export.ps1` 照样报「MSys/Mingw is not supported」。在 PowerShell 命令开头先 `Remove-Item Env:\MSYSTEM -ErrorAction SilentlyContinue` 再跑 export 即可（已实测可用）。
- **网页改动后跑一次回归测试**：`node tools/check_transfer_rgb565.js`，确保 RGB565 转换和字节序没被改坏。