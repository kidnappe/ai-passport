# 换动态头像 · 零基础跟我做

> 动态头像是**编译进固件**的逐帧精灵，换头像 = 生成素材 → 重新编译 → 烧录。
> **本平台一次只放一个人物**（引擎固定显示总表里的第一个），所以流程就是"准备好你这一个角色的帧，
> 让它成为唯一的那一个"。全程 4 条命令。

---

## 0. 前置检查（一次性）

1. **Python 3**：终端里 `python --version` 能打印版本即可。
2. **装 Pillow**（切图脚本要用）：
   ```bash
   pip install pillow
   ```
3. **ESP-IDF 5.5.3 环境**：只用 `.\tools\build.ps1` 构建即可，它已封装好环境（见文末"构建"）。
4. 在**仓库根目录 `o-platform/`** 下执行下面所有命令。

---

## 1. 做一个角色：去生成器捏 → 下载整表
1. 浏览器打开 [Universal LPC 角色生成器](https://sanderfrenken.github.io/Universal-LPC-Spritesheet-Character-Generator/)：
2. 捏好你的角色（身体 / 发型 / 衣服…）。
3. 点 **Download sheet**，得到一张**整表 PNG**。
4. 给它起个**英文小写短名**（只用字母，例：`hero`），下面命令都用它。

## 2. 切片（整表 → 每个动作每帧的小图）

把整表 PNG 放到 `o-platform/` 目录，然后（新手**只用中性动作**最稳，不用武器）：
```bash
python tools/lpc2pet.py 整表.png hero --only stand,walk,walkfront
```
产物：`humans/hero/` 下若干 `hero_<动作>_<帧号>_<方向>_<运动>.png`。

## 3. 转换（PNG → C 数组 + 生成总表）

```bash
python tools/prep_pet.py --src humans/hero
```
产物：`components/human_display/human/hero/human_hero_*.c/.h`，并重写总表
`components/human_display/human/human_manifest.h`。

## 4. 只留你这一个角色，然后编译 + 烧录

本平台只显示一个角色（`human_defs[0]`，且总表按角色名字母序取第一个）。为简单起见，**删掉别的人物，
只保留你的**：
```bash
# Windows 资源管理器或命令行删除这几个旧角色目录（如果存在）
#   humans/ 下：除 hero 以外的子目录
#   components/human_display/human/ 下：除 hero 以外的子目录
```
然后编译 + 烧录：
```powershell
.\tools\build.ps1 -Flash      # 编译并烧到 COM6
```
**看结果**：设备回主页，头像区就是你的人物，待机/走动随机切换动画。

> 完成后，第 2/3 步可反复做来微调（换动作、改帧、换角色名），每次重跑 `build.ps1 -Flash`。

---

## 想一次到位（可选进阶）

- **带攻击动作**：法师 `--only stand,walk,walkfront,caststaff`、战士加 `slash,thrust`、射手加 `shoot`。
  ⚠️ 没装备武器时这些只是空手动作；**别切 `idle/jump` 等扩展动画**（是裸身体图层，会着装闪烁）。
- **1x 小人 / 换抠图底色**：`python tools/prep_pet.py --src humans/hero --size 64`，
  抠底色用 `--key-color #1900FF --tolerance 50`（LPC 默认底色）。

---

## 参考：命名契约（手动放图 / 非 LPC 素材时）

`prep_pet.py` 只认这个名字，图从哪来不管：
`humans/<角色>/<角色>_<动作>_<帧号>_<方向r|l>_<运动>.png`（帧号从 1 起）。
- 方向：`r`=原样、`l`=自动镜像成朝右。
- 运动（必填，决定行为）：`idle` / `walkinplace` / `moveforward` / `sprintforward` / `moveup` / `sprintup`；
  同一动作各帧运动必须一致。例：`hero_walk_1_r_walkinplace.png`、`hero_idle_1_r_idle.png`。
- 手工裁帧要点：每帧裁成**同尺寸方形透明画布**、角色底边对齐画布底、所有帧裁同一矩形（否则会抖）。

## 参考：运动预设

| motion | 行为 | 帧周期 |
|---|---|---|
| `idle` | 原地循环，停 3s 随机换下一个静止动作 | 350ms |
| `walkinplace` | 原地踏步、不位移（头像区没水平空间，走路用它）| 160ms |
| `moveforward` / `sprintforward` | 水平慢/快走（头像区用不上）| 130 / 90ms |
| `moveup` / `sprintup` | 垂直小/大跳 | 120 / 110ms |

## 参考：规格与内存账

- 默认 `--size 128 --format rgb565a8`（64px 内容 2x，占满头像区）；1x 用 `--size 64`。
- 每帧约 48KB flash。**只放一个角色**、别堆太多帧，factory 分区 3MB、当前固件约 2.65MB。

## 常见坑

- CMake 已 `file(GLOB_RECURSE CONFIGURE_DEPENDS human/*.c)` 自动收集，**不用手改 CMakeLists**。
- `humans/` **根目录别放杂图**（没有 `_运动` 字段的图会让脚本报错）；按角色子目录 `--src humans/<名>` 跑。
- 只显示一个角色：引擎取 `human_defs[0]`、总表按角色名字母序——**只留一个角色**最省心。
- 想"运行时切换人物/头像"或"用传输页上传头像"目前**不支持**（素材是编译进固件的）。静态照片模式另见设置页开关。
