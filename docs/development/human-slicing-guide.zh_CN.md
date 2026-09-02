# 更换/新增首页动态头像（人物精灵）—— 使用手册

主页头像区是一个**会动的精灵**（`components/human_display`），它的帧素材**编译进固件**、
**不是运行时上传的**——换头像 = 生成素材 → 重新编译 → 烧录。本手册给出一条最短链路，
外加 LPC/命名/常见坑的详细参考。

---

## 最短链路（4 步）

```bash
# ① 切片：LPC 角色生成器导出的整表 PNG → 每动作每帧小 PNG
python tools/lpc2pet.py <整表>.png <角色名> --only stand,walk,walkfront,caststaff

# ② 转换：PNG → C 数组 + 自动重写动作总表 human_manifest.h
python tools/prep_pet.py --src humans/<角色名>

# ③ 编译（ESP-IDF 5.5.3；CMake 已自动收集 human/**/*.c，新增帧无需再改 CMakeLists）
idf.py build

# ④ 烧录并回主页验机
tools/flash.ps1
```

> ①② 在仓库根 `o-platform/` 下执行。① 也可跳过——只要 `humans/<角色名>/` 里放着**按命名契约**
> 取好名的 PNG（可手工裁），直接从 ② 开始即可（见"变体"）。

**只显示一个角色**：引擎固定用 `human_defs[0]`，而总表按**角色名字母序**生成——谁排第一谁上屏。
只想换一个主角，就让你要的角色名在字母序最前（或只放这一个角色）。想支持运行时切换需另加选择器。

---

## 命名契约（`prep_pet.py` 只认这个，图从哪来它不管）

`humans/<角色名>/<角色名>_<动作>_<帧号>_<方向>_<运动>.png`，帧号从 1 起：
- 方向：`r`=原样、`l`=自动水平镜像成朝右（默认朝右）。
- 运动 motion **必填**（决定引擎行为）：`idle` / `walkinplace` / `moveforward` /
  `sprintforward` / `moveup` / `sprintup`。
- 例：`mage_walk_1_r_walkinplace.png`、`mage_caststaff_3_r_idle.png`。
- 同一动作所有帧的 motion 必须一致，否则脚本报错退出。

## 运动预设（行为 / 帧周期）

| motion | 行为 | 帧周期 | 头像区可用性 |
|---|---|---|---|
| `idle` | 原地循环，停 3s 后随机换下一个静止动作 | 350ms | ✅ 常用 |
| `walkinplace` | 走动帧原地踏步、不位移 | 160ms | ✅ 常用（头像区没有水平走动空间）|
| `moveforward` / `sprintforward` | 水平慢走/快走 | 130/90ms | ❌ 需 >64px 走位，头像区用不上 |
| `moveup` / `sprintup` | 垂直小跳/大跳 | 120/110ms | ✅ |

> 头像区约 102×140、精灵 2x 后约 100×118，没有水平走位空间——"走路"请用 `walkinplace`。

## LPC 表要点（用 `lpc2pet.py` 时）

- 生成器：`sanderfrenken.github.io/Universal-LPC-Spritesheet-Character-Generator`，捏好角色 →
  **Download sheet**。
- 行序固定：每动作占 4 行，方向顺序 **上/左/下/右**（第 4 行朝右、第 3 行面向屏幕）。
- 动作取舍按职业配：法师 `caststaff`、战士 `slash/thrust`、射手 `shoot`；`stand/walk/walkfront` 中性、都留。
- ⚠️ **别切 idle/jump 等"扩展动画"行**：衣服图层只覆盖核心动画，扩展动画是裸身体，混进来会"着装闪烁"。
- 没装备武器时 slash/thrust/shoot 只是空手动作，意义不大，宁可不放。
- 想换动作集/帧挑选规则，编辑 `lpc2pet.py` 顶部 `LPC_ROWS`（`pick` 选帧、`face` 选朝向行、
  `col0/stride` 供表尾自定义宽间距动画区）。

## 规格与内存账

- `prep_pet.py` 默认 `--size 128 --format rgb565a8`：64px 内容最近邻 2x → 128×128，占满头像区。
  想要 1x 小人传 `--size 64`。抠背景色默认 `--key-color #1900FF`（LPC 洋红/蓝底），容差 50。
- 128×128 rgb565a8 每帧 ≈48KB flash。参考：mage 17 帧 ≈816KB；factory 分区 3MB，当前固件约
  2.65MB、剩余 ~352KB——**别塞太多角色/帧，会爆分区**。

---

## 变体：不用 LPC、纯手工裁帧

1. 每帧裁成**同尺寸方形透明画布**（如 64×64），角色底边对齐画布底（脚底线一致），所有帧裁同一矩形
   （否则帧间会抖）。
2. 按上面命名契约取名，放进 `humans/<角色名>/`。
3. 从"最短链路"第 ② 步继续。

## 变体：合成帧（源表没有现成动作时）

如 mage 的 `roll`：用 `crouch` 段切出团身球帧，再绕中心 90° 步进 `transpose`（像素无损重排）
生成旋转帧，按 `<名>_roll_<N>_<r>_<motion>.png` 命名放进 `humans/<名>/` 即可。

## 常见坑（都踩过）

- CMake：现已 `file(GLOB_RECURSE CONFIGURE_DEPENDS human/*.c)` 自动收集，**不再需要手改 CMakeLists**；
  重生成/删帧后直接 build 即可。
- `humans/` **根目录别放杂图**（无 motion 字段的图会报错）；`prep_pet.py` 按**角色子目录**跑
  （`--src humans/<名>`）即可避开。
- `prep_pet.py` 清单写到 `{out}/human_manifest.h`；组件根目录别留同名旧清单（相对 include 会优先命中旧的）。
- 2026-08-30 重构前旧名（`pets/`、`pet_manifest.h`、`pet_*` 符号）已全部改名为 `human_*`，见
  `folder-structure.zh_CN.md`；老文档提到这些时按新名对号入座。
