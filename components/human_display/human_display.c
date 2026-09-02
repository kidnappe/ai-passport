#include "human_display.h"
#include "human/human_manifest.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#define TICK_MS    40
#define WALK_INSET 2

typedef struct {
    lv_obj_t        *img;
    const human_def_t *def;
    uint8_t          act;
    uint8_t          frame;
    uint16_t         frame_acc;
    uint16_t         hold_acc;
    int16_t          x;
    int8_t           dir;
    bool             face_right;
    uint8_t          drawn_act;
    uint8_t          drawn_frame;
    bool             drawn_face;
} human_instance_t;

static lv_timer_t    *s_tick;
static lv_obj_t      *s_img;
static human_instance_t s_pet;
static bool           s_running;

static uint16_t s_scale256;
static int16_t  s_draw_w;
static int16_t  s_draw_h;
static int16_t  s_base_y;
static int16_t  s_walk_min;
static int16_t  s_walk_max;

static void human_draw(void)
{
    if (s_pet.act == s_pet.drawn_act && s_pet.frame == s_pet.drawn_frame &&
        s_pet.face_right == s_pet.drawn_face) {
        return;
    }
    lv_image_set_src(s_img, s_pet.def->actions[s_pet.act].frames[s_pet.frame]);
    s_pet.drawn_act   = s_pet.act;
    s_pet.drawn_frame = s_pet.frame;
    s_pet.drawn_face  = s_pet.face_right;
}

static uint8_t pick_rest(const human_def_t *def)
{
    if (def->rest_n == 0) return 0xFF;
    return def->rest_pool[lv_rand(0, def->rest_n - 1)];
}

static uint8_t pick_move(const human_def_t *def, uint8_t cur)
{
    if (def->move_n == 0) return 0xFF;
    if (def->move_n == 1) return def->move_pool[0];
    uint8_t i;
    do { i = def->move_pool[lv_rand(0, def->move_n - 1)]; } while (i == cur);
    return i;
}

static uint8_t pick_rest_other(const human_def_t *def, uint8_t cur)
{
    if (def->rest_n == 0) return 0xFF;
    if (def->rest_n == 1) return (def->rest_pool[0] == cur) ? 0xFF : def->rest_pool[0];
    uint8_t i;
    do { i = def->rest_pool[lv_rand(0, def->rest_n - 1)]; } while (i == cur);
    return i;
}

static void enter_act(uint8_t act)
{
    s_pet.act       = act;
    s_pet.frame     = 0;
    s_pet.frame_acc = 0;
    s_pet.hold_acc  = 0;
    lv_obj_set_y(s_img, s_base_y);
    human_draw();
}

static uint8_t start_act(const human_def_t *def)
{
    return (def->move_n > 0) ? def->move_pool[0] : 0;
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    const human_action_t *def = &s_pet.def->actions[s_pet.act];

    s_pet.frame_acc += TICK_MS;
    if (s_pet.frame_acc >= def->frame_ms) {
        s_pet.frame_acc = 0;
        s_pet.frame = (uint8_t)((s_pet.frame + 1) % def->n_frames);
        human_draw();
    }

    if (!def->stationary) {
        s_pet.x += (int16_t)(s_pet.dir * (int8_t)def->step_px);
        if (s_pet.x <= s_walk_min) {
            s_pet.x = s_walk_min;
            uint8_t r = pick_rest(s_pet.def);
            if (r != 0xFF) enter_act(r);
            else { s_pet.dir = 1; human_draw(); }
        } else if (s_pet.x >= s_walk_max) {
            s_pet.x = s_walk_max;
            uint8_t r = pick_rest(s_pet.def);
            if (r != 0xFF) enter_act(r);
            else { s_pet.dir = -1; human_draw(); }
        }
        lv_obj_set_x(s_img, s_pet.x);

        if (s_pet.def->move_n > 1 && lv_rand(0, 299) == 0) {
            uint8_t m = pick_move(s_pet.def, s_pet.act);
            if (m != 0xFF) enter_act(m);
        }
    } else {
        s_pet.hold_acc += TICK_MS;
        if (s_pet.hold_acc >= def->hold_ms) {
            s_pet.dir = (int8_t)-s_pet.dir;
            s_pet.face_right = (s_pet.dir > 0);
            human_draw();
            uint8_t m = pick_move(s_pet.def, s_pet.act);
            if (m != 0xFF) {
                enter_act(m);
            } else {
                uint8_t r = pick_rest_other(s_pet.def, s_pet.act);
                enter_act((r != 0xFF) ? r : s_pet.act);
            }
        }
    }

    if (def->hop_max > 0) {
        uint8_t n = def->n_frames;
        int32_t dy = 0;
        if (n > 1) {
            int32_t f   = s_pet.frame;
            int32_t num = f * (n - 1 - f);
            int32_t den = (int32_t)(n - 1) * (n - 1);
            dy = -((int32_t)def->hop_max * 4 * num) / den;
        }
        lv_obj_set_y(s_img, s_base_y + (int16_t)dy);
    }
}

bool human_display_start(const human_display_cfg_t *cfg)
{
    if (s_running || !cfg || !cfg->parent) return false;

    int side = cfg->box_w < cfg->box_h ? cfg->box_w : cfg->box_h;
    int scale_n = side / human_SPRITE_W;
    if (scale_n < 1) scale_n = 1;
    s_scale256 = (uint16_t)(256u * (uint32_t)scale_n);
    s_draw_w   = (int16_t)(human_SPRITE_W * scale_n);
    s_draw_h   = (int16_t)(human_SPRITE_H * scale_n);

    s_base_y   = (int16_t)(cfg->box_h - s_draw_h);

    if (s_draw_w >= cfg->box_w) {
        s_walk_min = 0;
        s_walk_max = 0;
    } else {
        s_walk_min = WALK_INSET;
        s_walk_max = (int16_t)(cfg->box_w - s_draw_w - WALK_INSET);
        if (s_walk_max < s_walk_min) s_walk_max = s_walk_min;
    }

    s_pet.def        = &human_defs[0];
    s_pet.dir        = 1;
    s_pet.face_right = true;
    s_pet.x          = (s_draw_w >= cfg->box_w)
                       ? (int16_t)((cfg->box_w - s_draw_w) / 2)
                       : s_walk_min;
    s_pet.drawn_act  = 0xFF;
    s_pet.drawn_frame = 0xFF;
    s_pet.drawn_face = true;

    s_img = lv_image_create(cfg->parent);
    lv_obj_set_size(s_img, s_draw_w, s_draw_h);
    lv_image_set_inner_align(s_img, LV_IMAGE_ALIGN_CENTER);
    lv_image_set_scale(s_img, s_scale256);
    lv_obj_set_pos(s_img, s_pet.x, s_base_y);

    enter_act(start_act(s_pet.def));

    s_tick = lv_timer_create(tick_cb, TICK_MS, NULL);
    s_running = true;
    return true;
}

void human_display_pause(void)
{
    if (s_tick) {
        lv_timer_pause(s_tick);
    }
}

void human_display_resume(void)
{
    if (s_tick) {
        lv_timer_resume(s_tick);
    }
}

void human_display_stop(void)
{
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    if (s_img)  { lv_obj_delete(s_img);    s_img  = NULL; }
    s_running = false;
}