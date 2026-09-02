#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""lpc2pet.py —— Universal LPC 精灵图表 → 动态帧宠物素材切片器

输入: LPC 角色生成器导出的整张表 (行表来自生成器 whichAnim 下拉的 data-row 权威数据)
输出: <out>/<宠物名>/*.png  命名契约 PNG (名_动作_帧_r_运动.png), 供 prep_pet.py 消费

切片规则:
 1) 行表驱动: 每个动作的起始行/帧数来自生成器 UI 数据 (非内容猜测);
 2) 右朝向 = 动作块内第 4 行 (LPC 方向序: 上/左/下/右);
 3) 同动作各帧取联合内容包围盒裁剪 (帧间对齐不破坏), 贴回 64×64 画布
    水平居中、底边锚底 (脚底线一致), 供 png2lvgl 1:1 转换。
"""
import argparse
import os
import sys

from PIL import Image

# 行表: 动作名 → (起始行, 块行数, 帧挑选下标, 契约动作名, motion, 朝向行偏移)
# 起始行/帧数来自生成器 whichAnim 下拉 data-row/data-cycle (权威)
# face = 动作块内朝向行偏移, LPC 方向序 上0/左1/正面2/右3, 缺省 3 (朝右)
# 注意: LPC 衣服图层只覆盖核心动画 (walk/slash/spellcast 等);
# idle/jump 等扩展动画只有裸身体图层, 混用会导致着装闪烁, 故只取 walk。
# walk 帧 0 复用为 1 帧的 stand 静止动作 (rest 池), 走走停停的节奏。
LPC_ROWS = {
    "stand":     {"row": 8,  "rows": 4, "pick": [0],       "action": "stand",     "motion": "idle"},
    "walk":      {"row": 8,  "rows": 4, "pick": [0, 2, 4, 6, 8], "action": "walk",      "motion": "walkinplace"},
    "walkfront": {"row": 8,  "rows": 4, "pick": [0, 2, 4, 6, 8], "action": "walkfront", "motion": "walkinplace", "face": 2},
    "slash":     {"row": 12, "rows": 4, "pick": [0, 2, 4], "action": "slash",     "motion": "idle"},
    "spellcast": {"row": 0,  "rows": 4, "pick": [0, 1, 2], "action": "spellcast", "motion": "idle"},
    # caststaff: 表尾宽间距区 (col0=1 起, 每 3 列一帧) 的持杖施法, 正面朝向
    "caststaff": {"row": 53, "rows": 1, "pick": [0, 1, 2, 3, 4, 5], "action": "caststaff", "motion": "idle",
                  "col0": 1, "stride": 3},
    # crouch: 第 20 行 (单行, 穿衣图层覆盖) 的俯身/团身 3 帧, 供 roll 合成入场段
    "crouch":    {"row": 20, "rows": 1, "pick": [3, 4, 5], "action": "crouch",    "motion": "idle"},
    "thrust":    {"row": 4,  "rows": 4, "pick": [0, 3, 6], "action": "thrust",    "motion": "idle"},
    "shoot":     {"row": 16, "rows": 4, "pick": [0, 2, 4], "action": "shoot",     "motion": "idle"},
}


def slice_anim(im, spec):
    """切朝向行 (face 偏移, 缺省块内第 4 行=朝右; face=2 切正面)。
    col0/stride: 宽间距区 (表尾自定义动画) 的起始列与列步长, 缺省 0/1 连续帧。
    全动作联合包围盒裁剪 (帧间对齐不破坏),
    贴回 64×64 画布水平居中、底边锚底 (脚底线一致)"""
    row = spec["row"] + spec.get("face", spec["rows"] - 1)
    y0 = row * 64
    stride = spec.get("stride", 1)
    x_off = spec.get("col0", 0) * 64
    cell_w = 64 * stride
    picks = [c for c in spec["pick"]
             if x_off + c * cell_w + cell_w <= im.size[0]]
    cells = []
    for cx in picks:
        cell = im.crop((x_off + cx * cell_w, y0,
                        x_off + cx * cell_w + cell_w, y0 + 64))
        if cell.getbbox() is not None:
            cells.append(cell)
    if not cells:
        return []

    # 联合包围盒: 所有帧裁同一矩形, 保持帧间相对对齐
    l, t, r, b = 10**9, 10**9, -1, -1
    for c in cells:
        bb = c.getbbox()
        l, t = min(l, bb[0]), min(t, bb[1])
        r, b = max(r, bb[2]), max(b, bb[3])
    bw, bh = r - l, b - t
    if bw > 64 or bh > 64:
        sys.exit(f"动作 {spec['action']} 内容 {bw}x{bh} 超出 64x64 画布 "
                 f"(宽间距动画的横扫帧放不下, 换朝向或减帧)")

    frames = [c.crop((l, t, r, b)) for c in cells]
    bw, bh = r - l, b - t

    out = []
    for f in frames:
        canvas = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
        canvas.paste(f, ((64 - bw) // 2, 64 - bh))   # 水平居中, 底边锚底
        out.append(canvas)
    return out


def main():
    ap = argparse.ArgumentParser(description="LPC 精灵图表 → 动态帧宠物素材切片器")
    ap.add_argument("sheet", help="LPC 导出整表 PNG")
    ap.add_argument("name", help="宠物名 (输出文件夹/命名前缀)")
    ap.add_argument("--out", default="humans", help="输出根目录 (每人物一个子文件夹)")
    ap.add_argument("--only", default="", help="逗号分隔, 只切这些动作 (LPC_ROWS 键); 缺省=全表。按角色配动作: 法师 --only stand,spellcast")
    args = ap.parse_args()

    im = Image.open(args.sheet).convert("RGBA")
    print(f"图表: {im.size[0]}×{im.size[1]} ({im.size[0]//64}×{im.size[1]//64} 格)")

    outdir = os.path.join(args.out, args.name)
    os.makedirs(outdir, exist_ok=True)
    for old in os.listdir(outdir):
        if old.endswith(".png"):
            os.remove(os.path.join(outdir, old))

    only = {s.strip() for s in args.only.split(",") if s.strip()}
    unknown = only - set(LPC_ROWS)
    if unknown:
        sys.exit(f"--only 含未知动作: {', '.join(sorted(unknown))} (可选: {', '.join(LPC_ROWS)})")

    made = []
    for anim, spec in LPC_ROWS.items():
        if only and anim not in only:
            continue
        frames = slice_anim(im, spec)
        for i, fim in enumerate(frames):
            fname = f"{args.name}_{spec['action']}_{i + 1}_r_{spec['motion']}.png"
            fim.save(os.path.join(outdir, fname))
            made.append(fname)
        print(f"  {anim}: 行 {spec['row']}+{spec.get('face', spec['rows'] - 1)} → {len(frames)} 帧 ({spec['motion']})")

    print(f"输出 {len(made)} 帧 → {outdir}/")
    for f in made:
        print("  OK", f)


if __name__ == "__main__":
    main()
