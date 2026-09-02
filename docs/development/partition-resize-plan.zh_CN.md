# 分区重划方案（最终定稿）

> 状态: **已实施** · 实施日期: 2026-08-30
> 目标: 扩大固件分区为后续功能留余量；数据分区缩小到 1MB 仅存放用户配置

## 一、背景与现状

8MB flash，原分区表:

| 分区 | 类型/子类型 | 偏移 | 大小 | 用途 |
|---|---|---|---|---|
| (bootloader+分区表) | — | 0x0000 | 64KB | 引导 |
| nvs | data/nvs | 0x9000 | 24KB | WiFi 凭证、配置 |
| phy_init | data/phy | 0xF000 | 4KB | 射频校准 |
| factory | app/factory | 0x10000 | 3,072KB | 固件本体 |
| appfs | data/fat | 0x310000 | 5,056KB | FAT 数据盘 |

## 二、最终分区表（已实施）

| 分区 | 类型/子类型 | 偏移 | 大小 | 变化 |
|---|---|---|---|---|
| nvs | data/nvs | 0x9000 | 24KB | 不变 |
| phy_init | data/phy | 0xF000 | 4KB | 不变 |
| factory | app/factory | 0x10000 | **7,104KB (0x6F0000)** | +4,032KB，余量 ≈ 4.9MB |
| data | data/fat | **0x700000** | **1,024KB (0x100000)** | 滚动到末尾，容量仅存用户数据 |

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x6F0000,
data,     data, fat,     0x700000, 0x100000,
```

### 决策依据

- 动画帧（22 帧 × 48KB = 1,058KB）**留在固件编译**，不外置到 data 分区。
  ESP32-C3 仅 ~180KB 可用堆，48KB 换帧缓冲会撑爆 BLE+WiFi 初始化，且 LVGL 文件系统驱动
  无法零拷贝读取 FAT 文件。
- data 分区仅存用户配置（昵称/学院/头像等，~50KB），1MB 绰绰有余。
- factory 7,104KB，当前固件 ~2.96MB，余量 ~4.1MB（66% 空闲），可容纳约 85 帧额外动画。
- 标签同步改为 `data`，挂载代码已同步一处字符串（`passport_storage.c:86`）。

## 三、实施记录

1. 改 `partitions.csv`（factory 0x6F0000 / data 0x700000 0x100000）
2. 改 `passport_storage.c:86` 挂载标签 `"appfs"`→`"data"`
3. `idf.py fullclean` → `idf.py build` → 烧写
4. 用户数据（昵称/学院/头像等）从 `references/appfs-backup-20260830/` 中提取，
   通过 `tools/make_data_image.py` 生成 WL+FAT 分区镜像，与固件同时烧入。
5. **回归验证结果**:
   - 开机挂载成功、主页头像/字段正常显示
   - 宠物模式动画正常播放（帧编译在固件中）
   - BLE 广播正常（堆余 53KB）
   - WiFi 初始化成功
   - 设备详情页显示正确

## 四、后续可选项（不在本次范围）

- 中文字库外置（412KB）到 data 分区: +0.5-1 人日
- 若日后重提 OTA: 需重新规划（双槽方案与本表冲突）
- 若未来走向重负载（更多角色 + 全量字库 + 录音），需重新议表

## 五、风险

| 风险 | 缓解 |
|---|---|
| 重分区后用户数据丢失 | 已知代价，烧写后重新录入即可（几分钟的人工操作） |
| 固件未来超 7MB | 编译期报错，可见失败；当前 2.96MB，余量 4.1MB |
| FAT 掉电损毁 | 写入极少（改昵称/传头像才写）；必要时可加临时文件+改名写入 |
| 分区表与 build 缓存不一致 | `idf.py fullclean` 后重建，流程里已包含 |
