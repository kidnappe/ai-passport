# 首页人物（精灵）动作切片手册（手动流程）

从 LPC 角色表到固件里会动的人物，共 5 步。工具链（目录命名遵循
[folder-structure.zh_CN.md](folder-structure.zh_CN.md)：源 PNG 在 `humans/`，
生成数据在 `components/human_display/human/`，符号前缀 `human_`）：

```
LPC 生成器导出整表 PNG
      │  ① tools/lpc2pet.py        切片: 大图 → 每动作每帧小 PNG (humans/<名>/)
      │  ② tools/prep_pet.py       转换: PNG → C 数组 + 自动生成动作总表 human_manifest.h
      │  ③ 手改 CMakeLists.txt     把新 .c 注册进编译
      │  ④ idf.py build            编译 → build/o-platform.bin
      ▼  ⑤ tools/flash.ps1         烧写 → 回主页验机
```

## 第 0 步：导出 LPC 角色表

1. 浏览器打开 Universal LPC 角色生成器
   （`sanderfrenken.github.io/Universal-LPC-Spritesheet-Character-Generator`）。
2. 捏好角色（身体/发型/衣服……），点 **Download sheet** 保存整表 PNG。
3. 给角色起个英文短名（如 `mage`），后面所有命令都用它。
4. 记下行号（权威数据，来自生成器 whichAnim 下拉的 data-row）：

   | 动作 | 起始行 | 块行数 | 帧数 |
   |---|---|---|---|
   | spellcast（施法） | 0 | 4 | 7 |
   | thrust（突刺） | 4 | 4 | 8 |
   | walk（走动） | 8 | 4 | 9 |
   | slash（挥砍） | 12 | 4 | 6 |
   | shoot（射击） | 16 | 4 | 6 |

   每个动作占 4 行，方向顺序固定为 **上 / 左 / 下 / 右**（第 4 行 = 朝右，
   第 3 行 = 面向屏幕）。

> **坑**：不要用 idle / jump 等"扩展动画"行。衣服图层只覆盖上表 5 个核心动画，
> 扩展动画只有裸身体图层，混进表里会"着装闪烁"（穿戴瞬间消失又出现）。

## 第 1 步：挑动作 → 切片

**动作取舍规则**：攻击动作按职业配（法师=caststaff 持杖施法，战士=slash/thrust，
射手=shoot）；**stand / walk / walkfront 是中性动作，各职业都保留**。
编辑 `tools/lpc2pet.py` 顶部的 `LPC_ROWS` 表决定切什么：

```python
"walk":      {"row": 8,  "rows": 4, "pick": [0, 2, 4, 6, 8], "action": "walk",      "motion": "walkinplace"},
"walkfront": {"row": 8,  "rows": 4, "pick": [0, 2, 4, 6, 8], "action": "walkfront", "motion": "walkinplace", "face": 2},
"caststaff": {"row": 53, "rows": 1, "pick": [0, 1, 2, 3, 4, 5], "action": "caststaff", "motion": "idle",
              "col0": 1, "stride": 3},
#         └起始行        └块行数  └挑哪些帧(下标从0)     └输出文件用的动作名    └行为(见下)  └朝向行 └起始列/列步长
```

- `pick` 控制保留哪几帧（0 起），少挑帧 = 省 flash 也更省事。
- `face` 选朝向行：上0 / 左1 / 正面2 / 右3，缺省 3（朝右）；`face: 2` 即面向屏幕。
  `walkfront` = 正面原地走，引擎里就是一个普通的静止动作。
- `col0`/`stride`：表尾"宽间距自定义动画"区（帧每隔 3 列一个）专用，
  如 caststaff（持杖施法，row 53 正面 / row 56 朝右）。横扫帧内容可宽达 187px，
  超出 64 画布脚本会直接报错，只切竖持杖的朝向。
- `motion` 决定引擎行为（定义在 `tools/png2lvgl.py` 的 `MOTION_PRESETS`）：

  | motion | 行为 | 帧周期 | 备注 |
  |---|---|---|---|
  | `idle` | 原地循环 | 350ms | 停 3 秒后随机换下一个静止动作 |
  | `walkinplace` | 原地踏步 | 160ms | 走动帧速原地播放，不位移 |
  | `moveforward` | 水平慢走 | 130ms | 需要 >64px 的走动空间，当前用不上 |
  | `sprintforward` | 水平快走 | 90ms | 同上 |
  | `moveup` | 垂直小跳 | 120ms | hop 6px |
  | `sprintup` | 垂直大跳 | 110ms | hop 12px |

  头像区只有 102×140、精灵 2x 放大后 ~100×118，**没有水平走动空间**，
  走动一律用 `walkinplace`（朝右或正面踏步）。

运行切片（仓库根目录 `o-platform/` 下；`--out` 缺省就是 `humans/`）：

```bash
# 法师: 持杖施法 + 站立 + 双向原地走 (当前固件在用, 已切好)
python tools/lpc2pet.py <导出的整表>.png mage --only stand,walk,walkfront,caststaff

# 战士: 挥砍/突刺 + 站立 + 双向原地走
python tools/lpc2pet.py <导出的整表>.png knight --only stand,walk,walkfront,slash,thrust
```

不带 `--only` 就切 LPC_ROWS 里全部动作。没装备武器的角色，slash/thrust/shoot
只有空手身体动作（没有兵器画面），切出来意义不大，宁可不放。

输出 `humans/<名>/<名>_<动作>_<帧号>_<r|l>_<motion>.png`（帧号从 1 起；
dir 字段 r=素材原样，l=自动镜像成朝右）。

> **坑**：`humans/` 根目录别放杂图（`_action_menu.png`、`cowboy_contact.png`
> 之类没有 motion 字段的图，现在就躺在那里）。`prep_pet.py` 按 `--src` 目录
> glob 一层，杂图会直接报错——所以 prep_pet 按**人物子目录**跑（见第 2 步）。

## 第 2 步：转 C 数组 + 生成动作总表

按人物子目录跑（默认参数已对齐当前结构，不用再加选项）：

```bash
python tools/prep_pet.py --src humans/mage
```

- 每个 PNG → `components/human_display/human/mage/human_mage_<动作>_<帧>.c/.h`。
- 自动重写 `components/human_display/human/human_manifest.h`（动作总表，**勿手改**）。
- 默认 `--size 128 --format rgb565a8`：64px 内容最近邻 2x 到 128×128，像素锐利，
  占满头像区。想回 1x 小人传 `--size 64`。
- 同一动作各帧的 motion 必须一致，否则脚本报错退出。

## 第 3 步：注册进编译（唯一一处手改代码）

`components/human_display/CMakeLists.txt` 的 SRCS 是**显式列表，不会自动 glob**。
把新动作的每个 `.c` 加进去，路径必须带人物子目录（漏层会报 cannot find source file）：

```cmake
idf_component_register(
    SRCS "human_display.c"
         "human/mage/human_mage_spellcast_1.c"
         ...每个帧一个 .c...
```

引擎固定用 `human_defs[0]`（`human_display.c` 的 `human_display_start`），而总表
按人物名**字母序**生成——谁的名字排前谁上屏。想换主角，要么改名让目标人物排第一，
要么生成后核对注册表顺序。

## 第 4、5 步：编译、烧写、验机

```bash
idf.py build        # 需要 ESP-IDF 5.5.3 环境
tools/flash.ps1     # 直接烧 build/ 里的现成镜像（PowerShell）
```

Git Bash 里跑 idf.py 前先去掉 MSYS 变量（ESP-IDF 拒绝 MSys 环境）：

```bash
env -u MSYSTEM powershell -NoProfile -Command '. "E:\code\code tools\esp-idf-v5.5.3\export.ps1"; idf.py build'
```

验机：设备回主页，头像区出现人物，动作到期后随机切换。资源账参考：
128×128 rgb565a8 每帧 ≈48KB；mage 17 帧（含持杖施法 6 帧）≈816KB；分区 3MB，
当前固件约 2.65MB，剩余 ~352KB。

## 变体：合成帧（mage 的 ROLL 为例）

源表没有现成滚转帧时可以合成：`crouch` 条目切出第 20 行的穿衣团身段
（俯身/深蹲/团身球），再把团身球帧绕自身中心做 90° 步进 `transpose`
（像素无损重排）生成旋转帧，按 `<名>_roll_<N>_<dir>_<motion>.png` 契约命名
放进 `humans/<名>/` 即可。入场实拍帧 + 旋转球循环 = 翻滚动作。

## 变体：不用脚本、纯手工切帧

`prep_pet.py` 只认**契约命名**的 PNG，不关心图从哪来。图像编辑器里手动裁帧也行：

1. 每帧裁成**同尺寸方形透明画布**（如 64×64），角色底边对齐画布底（脚底线一致），
   所有帧裁同一个矩形（帧间才不会抖）。
2. 命名 `<名>_<动作>_<帧号>_<r|l>_<motion>.png`，帧号从 1 起，
   如 `mage_walk_1_r_walkinplace.png`；`l` 表示朝左素材（转换时自动镜像成朝右）。
3. 放进 `humans/<名>/`，从上面第 2 步继续。

## 历史坑清单（都已踩过）

- `prep_pet.py` 把清单写在 `{out}/human_manifest.h`；组件根目录不能留同名旧清单
  （相对 include 会优先命中旧文件）。
- CMake SRCS 路径漏掉 `human/<名>/` 层 → cannot find source file。
- `--src` 目录下混入无 motion 字段的杂图 → 解析报错（humans/ 根目录现有
  `_action_menu.png`、`cowboy_contact.png` 两张，按人物子目录跑即可避开）。
- LPC 扩展动画（idle/jump）是裸身图层，混入核心动画会着装闪烁。
- LPC 没装备武器时 slash/thrust/shoot 是空手动作；法杖/武器是独立图层，
  只跟随 stand/walk。
- 帧挑选后脚本做**全动作联合包围盒裁剪**（所有帧裁同一矩形）+ 底边锚底，
  这是对齐的关键，手工路径必须自己保证同样的事。
- 2026-08-30 重构前的旧路径（`pets/`、`pet_manifest.h`、`pet_*` 符号、
  `FoloToy-AI-Passport.bin`）已全部改名，见 folder-structure 文档；网上或旧文档
  提到这些路径时按新名对号入座。
