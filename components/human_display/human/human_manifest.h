/* 由 prep_pet.py 自动生成 —— 勿手改 */
#ifndef human_MANIFEST_H
#define human_MANIFEST_H

#include "lvgl.h"

#define human_HAS_BG  0

#define human_SPRITE_W  128
#define human_SPRITE_H  128

typedef enum { MOT_IDLE, MOT_MOVEFORWARD, MOT_SPRINTFORWARD, MOT_MOVEUP, MOT_SPRINTUP } human_mot_t;

typedef struct {
    const char *name;                        /* 显示标签 (动作名大写) */
    const lv_image_dsc_t *const *frames;    /* 帧指针表 */
    uint8_t  n_frames;
    uint16_t frame_ms;     /* 换帧周期 (ms) */
    uint16_t hold_ms;      /* 静止持续; 0 = 移动到撞墙 */
    human_mot_t motion;      /* 运动学类型 */
    uint8_t  step_px;      /* 每 tick 水平位移 */
    int8_t   hop_max;      /* 垂直跳跃幅度 (0 = 不跳) */
    bool     stationary;   /* true = 原地不动 */
} human_action_t;

typedef struct {
    const char *name;            /* 人物名 */
    const human_action_t *actions; /* 动作表 */
    uint8_t  act_count;          /* 动作数 */
    const uint8_t *rest_pool;    /* 静止动作下标池 */
    uint8_t  rest_n;             /* 静止池大小 */
    const uint8_t *move_pool;    /* 移动动作下标池 */
    uint8_t  move_n;             /* 移动池大小 */
} human_def_t;

/* ===== mage ===== */
#include "mage/human_mage_caststaff_1.h"
#include "mage/human_mage_caststaff_2.h"
#include "mage/human_mage_caststaff_3.h"
#include "mage/human_mage_caststaff_4.h"
#include "mage/human_mage_caststaff_5.h"
#include "mage/human_mage_caststaff_6.h"
#include "mage/human_mage_roll_1.h"
#include "mage/human_mage_roll_2.h"
#include "mage/human_mage_roll_3.h"
#include "mage/human_mage_roll_4.h"
#include "mage/human_mage_roll_5.h"
#include "mage/human_mage_roll_6.h"
#include "mage/human_mage_walk_1.h"
#include "mage/human_mage_walk_2.h"
#include "mage/human_mage_walk_3.h"
#include "mage/human_mage_walk_4.h"
#include "mage/human_mage_walk_5.h"
#include "mage/human_mage_walkfront_1.h"
#include "mage/human_mage_walkfront_2.h"
#include "mage/human_mage_walkfront_3.h"
#include "mage/human_mage_walkfront_4.h"
#include "mage/human_mage_walkfront_5.h"

static const lv_image_dsc_t *const mage_caststaff_frames[] = { &human_mage_caststaff_1, &human_mage_caststaff_2, &human_mage_caststaff_3, &human_mage_caststaff_4, &human_mage_caststaff_5, &human_mage_caststaff_6 };
static const lv_image_dsc_t *const mage_roll_frames[] = { &human_mage_roll_1, &human_mage_roll_2, &human_mage_roll_3, &human_mage_roll_4, &human_mage_roll_5, &human_mage_roll_6 };
static const lv_image_dsc_t *const mage_walk_frames[] = { &human_mage_walk_1, &human_mage_walk_2, &human_mage_walk_3, &human_mage_walk_4, &human_mage_walk_5 };
static const lv_image_dsc_t *const mage_walkfront_frames[] = { &human_mage_walkfront_1, &human_mage_walkfront_2, &human_mage_walkfront_3, &human_mage_walkfront_4, &human_mage_walkfront_5 };

static const human_action_t mage_actions[] = {
  /* CASTSTAFF */ { "CASTSTAFF", mage_caststaff_frames, 6, 350, 3000, MOT_IDLE, 0, 0, true },
  /* ROLL */ { "ROLL", mage_roll_frames, 6, 160, 3000, MOT_IDLE, 0, 0, true },
  /* WALK */ { "WALK", mage_walk_frames, 5, 160, 3000, MOT_IDLE, 0, 0, true },
  /* WALKFRONT */ { "WALKFRONT", mage_walkfront_frames, 5, 160, 3000, MOT_IDLE, 0, 0, true },
};
#define MAGE_ACT_COUNT  4

#define MAGE_HAS_REST  1
static const uint8_t mage_rest_pool[] = { 0, 1, 2, 3 };
#define MAGE_REST_N  (4)

#define MAGE_HAS_MOVE  0
static const uint8_t mage_move_pool[] = { 0 };
#define MAGE_MOVE_N  (0)


/* ===== 人物注册表 ===== */
static const human_def_t human_defs[] = {
  { "mage", mage_actions, MAGE_ACT_COUNT, mage_rest_pool, MAGE_REST_N, mage_move_pool, MAGE_MOVE_N },
};

#define human_DEFS_COUNT  (sizeof(human_defs) / sizeof(human_defs[0]))

#endif /* human_MANIFEST_H */