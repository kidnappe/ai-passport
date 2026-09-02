#pragma once

#include "lvgl.h"

typedef struct {
    lv_obj_t *parent;  /* 宿主容器（主页头像区） */
    int      box_w;    /* 头像区宽：几何派生的唯一尺寸源 */
    int      box_h;    /* 头像区高 */
} human_display_cfg_t;

/* 在 parent 内启动动态精灵。几何（缩放/地面线/走动边界）
 * 全部从 box_w/box_h 派生，见 human_display.c 顶部的四条派生规则。
 * 返回 true 表示启动成功，false 表示内存不足（调用方可回退到照片模式）。 */
bool human_display_start(const human_display_cfg_t *cfg);

/* 停止并清理（定时器/图片对象）。幂等。 */
void human_display_stop(void);

/* 暂停动画（停止定时器，保留图片对象）。幂等。 */
void human_display_pause(void);

/* 恢复动画（重启定时器）。幂等。 */
void human_display_resume(void);
