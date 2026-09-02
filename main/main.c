#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "passport_identity.h"
#include "passport_input_policy.h"
#include "passport_theme.h"
#include "passport_ui.h"
#include "passport_settings.h"
#include "passport_storage.h"
#include "ble_prov.h"
#include "wifi_ap_prov.h"
#include "wifi_sta.h"
#include "transfer_page.h"
#include "human_display.h"
#include "passport_voice.h"
#include "passport_ppt.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_task_wdt.h"
#include "nvs.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "lvgl.h"
#include "esp_partition.h"

static const char *TAG = "o-platform";
#define EVENT_QUEUE_DEPTH 16
#define SETTINGS_VALUE_ROW_COUNT 4
#define SETTINGS_LIST_ROW_COUNT (SETTINGS_VALUE_ROW_COUNT + 4)   /* +网络与连接/动态头像/设备信息/主题 */

typedef enum {
    VIEW_LAUNCHER = 0,
    VIEW_APPS,
    VIEW_SETTINGS,
    VIEW_DEVICE_INFO,
    VIEW_THEMES,
    VIEW_WIFI,
    VIEW_TRANSFER,
    VIEW_VOICE,
    VIEW_PPT,
} view_t;

typedef enum {
    EVENT_KEY = 1,
    EVENT_WIFI_CONNECT_DONE,
    EVENT_VOICE_TRANSCRIPT,
    EVENT_VOICE_LINK,
    EVENT_VOICE_ERROR,
} event_type_t;

typedef struct {
    event_type_t type;
    union {
        struct { bsp_btn_t btn; bsp_btn_ev_t ev; } key;
        struct { bool ok; } wifi_connect;
        struct { char text[128]; bool final; } voice_transcript;
        struct { bool up; } voice_link;
    } data;
} system_event_t;

static QueueHandle_t s_events;
static view_t s_view;
static view_t s_prev_view;
static passport_page_t *s_page;
static passport_ui_list_t *s_list;
static bool s_settings_editing;
static bool s_on_network_page;
static bool s_was_on_network;
static bool s_screen_off;
bool s_bt_enabled = true;
bool s_wifi_enabled = true;

static bool nvs_get_bool(const char *key, bool def) {
    nvs_handle_t h; bool val = def;
    if (nvs_open("pass_net", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0; nvs_get_u8(h, key, &v); val = (v != 0);
        nvs_close(h);
    }
    return val;
}
static void nvs_set_bool(const char *key, bool val) {
    nvs_handle_t h;
    if (nvs_open("pass_net", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, val ? 1 : 0); nvs_commit(h); nvs_close(h);
    }
}
static passport_theme_info_t s_themes[PASSPORT_MAX_INSTALLED_THEMES];
static size_t s_theme_count;
static passport_settings_wake_guard_t s_wake_guard;

/* WiFi provisioning state（destroy_native_view 也要清理这些，故前移声明） */
typedef enum { WIFI_INFO, WIFI_PROV, WIFI_DONE } wifi_view_t;
static wifi_view_t s_wifi_view;
static volatile int s_wifi_phase; /* 0=waiting, 1=connecting, 2=ok, 3=no_net, 4=fail */
static volatile bool s_wifi_cancel;
static volatile bool s_wifi_task_alive;
static lv_timer_t *s_wifi_poll;
static lv_obj_t *s_wifi_status;
static lv_obj_t *s_wifi_hint;

/* 语音输入(PTT 按住 ↓ 说话)视图状态;PTT 键为 DOWN,UP 留给全局返回上一层 */
typedef enum {
    VOICE_READY = 0,      /* 就绪:按住 ↓ 说话 */
    VOICE_RECORDING,      /* 录音中:松开发送 */
    VOICE_TRANSCRIBING,   /* 等桌面端转写:单击 ↓ 退出 */
} voice_state_t;
static voice_state_t s_voice_state;
static lv_obj_t *s_voice_hint;
static lv_obj_t *s_voice_text;
static lv_obj_t *s_voice_timer_label;
static lv_timer_t *s_voice_timer;      /* 录音计时(500ms) */
static lv_timer_t *s_voice_vu_timer;   /* 音量条更新(100ms) */
static lv_obj_t *s_voice_bar_bg;       /* 音量条背景 */
static lv_obj_t *s_voice_bar_fg;       /* 音量条前景(绿色) */
static uint32_t s_voice_start_tick;
#define VOICE_MIN_TALK_MS 500
static void voice_set_hint(const char *text);
static void voice_stop_timer(void);
static void voice_vu_cb(lv_timer_t *t);

/* PPT 遥控视图状态(进则配对/退则停;BLE 由 show_ppt 启动,destroy_native_view 停) */
static lv_obj_t *s_ppt_hint;      /* 连接状态提示 */
static lv_obj_t *s_ppt_action;    /* 最近操作反馈 */
static lv_obj_t *s_ppt_timer_label; /* 演讲计时(mm:ss) */
static lv_timer_t *s_ppt_timer;   /* 状态+计时刷新(500ms) */
static bool s_ppt_pres_running;   /* 计时是否在跑(首次 F5 后启动) */
static uint32_t s_ppt_pres_seconds;
#define PPT_ACTION_MS 2000
static void ppt_stop_timer(void);
static void ppt_refresh_status(void);

/* Forward declarations */
static void destroy_native_view(void);
static void show_home(void);
static void show_settings(void);
static void show_device_info(void);
static void show_themes(void);
static void show_wifi(void);
static void show_network(void);
static void show_transfer_page(void);
static void handle_transfer_key(bsp_btn_t btn, bsp_btn_ev_t ev);
static void destroy_transfer_page(void);
static void show_voice(void);
static void show_ppt(void);
/* ===== 二级应用页基础设施 =====
 * 移植进来的固件功能在此登记一条即可上列表，OK 键打开。
 * 打开回调自行建页并接管按键；全局「长按 OK 回主页」负责退出，
 * 若创建了 native 屏/定时器，在 destroy_native_view 的清理钩子里释放。 */
static const passport_setting_id_t SETTINGS_ROWS[SETTINGS_VALUE_ROW_COUNT] = {
    PASSPORT_SETTING_BRIGHTNESS,
    PASSPORT_SETTING_SCREEN_TIMEOUT,
    PASSPORT_SETTING_VOLUME,
    PASSPORT_SETTING_KEY_SOUND,
};

static const char *const SETTINGS_NAMES[SETTINGS_VALUE_ROW_COUNT] = {
    "屏幕亮度",
    "息屏时间",
    "系统音量",
    "按键音",
};

static lv_obj_t *s_home_screen;
static passport_status_bar_t *s_home_bar;
static lv_obj_t *s_home_content;
static lv_timer_t *s_home_timer;
static lv_obj_t *s_home_btn_apps;
static lv_obj_t *s_home_btn_settings;
static int s_home_selected;
static void *s_avatar_buf;
static lv_image_dsc_t *s_avatar_dsc;
static lv_obj_t *s_home_nickname;
static lv_obj_t *s_home_college;
static lv_obj_t *s_home_major;
static lv_obj_t *s_home_student_id;

typedef struct {
    const char *name;        /* 列表条目名 */
    void (*open)(void);      /* OK 打开 */
} builtin_app_t;

/* 未来移植的功能在此追加条目，例如:
 *   { "像素宠物", open_pet_playground }, */
static const builtin_app_t BUILTIN_APPS[] = {
    { "语音输入", show_voice },
    { "PPT 遥控", show_ppt },
};
static int builtin_app_count(void)
{
    /* 经由函数返回, 避免空注册表时 sizeof/sizeof 常量 0 触发 -Wtype-limits */
    return (int)(sizeof(BUILTIN_APPS) / sizeof(BUILTIN_APPS[0]));
}

static void show_apps(void)
{
    destroy_native_view();
    s_prev_view = s_view;
    s_page = passport_ui_page_create("应用", true, true);
    s_list = passport_ui_list_create(s_page, builtin_app_count() > 0 ? builtin_app_count() : 1);
    if (builtin_app_count() == 0) {
        passport_ui_list_add(s_list, "暂无应用");
    } else {
        for (int i = 0; i < builtin_app_count(); ++i)
            passport_ui_list_add(s_list, BUILTIN_APPS[i].name);
    }
    passport_ui_page_set_actions(s_page, builtin_app_count() ? "打开" : "", "主页");
    passport_ui_page_show(s_page);
    s_view = VIEW_APPS;
}

static void update_home_status(lv_timer_t *timer)
{
    (void)timer;
    /* 时间/星期/电量/蓝牙与 WiFi 图标全部由共用状态栏组件负责 */
    passport_status_bar_update(s_home_bar);
}

static void destroy_home(void)
{
    if (s_home_timer) lv_timer_delete(s_home_timer);
    s_home_timer = NULL;
    human_display_stop();
    passport_status_bar_delete(s_home_bar);
    s_home_bar = NULL;
    if (s_home_screen) lv_obj_delete(s_home_screen);
    s_home_screen = NULL;
    s_home_content = NULL; s_home_btn_apps = NULL; s_home_btn_settings = NULL;
    s_home_nickname = NULL;
    s_home_college = NULL; s_home_major = NULL; s_home_student_id = NULL;
    free(s_avatar_dsc);
    free(s_avatar_buf);
    s_avatar_dsc = NULL;
    s_avatar_buf = NULL;
}

static void show_home(void)
{
    destroy_native_view();
    destroy_home();

    const passport_theme_tokens_t *t = passport_theme_current();

    s_home_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_home_screen, BSP_LCD_W, BSP_LCD_H);
    lv_obj_set_style_bg_color(s_home_screen, lv_color_hex(t->background), 0);
    lv_obj_set_style_bg_opa(s_home_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_home_screen, 0, 0);
    lv_obj_set_style_pad_all(s_home_screen, 0, 0);
    lv_obj_remove_flag(s_home_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_home_bar = passport_status_bar_create(s_home_screen, PASSPORT_STATUS_BAR_HOME, NULL);

    s_home_content = lv_obj_create(s_home_screen);
    lv_obj_remove_flag(s_home_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_home_content, BSP_LCD_W, BSP_LCD_H - 28 - 72);
    lv_obj_set_pos(s_home_content, 0, 28);
    lv_obj_set_style_bg_color(s_home_content, lv_color_hex(t->background), 0);
    lv_obj_set_style_bg_opa(s_home_content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_home_content, 0, 0);
    lv_obj_set_style_pad_all(s_home_content, 0, 0);

    /* 字段纵向布局常量（头像下边界需与最后一行字段齐平，故先于头像确定）。
     * 昵称 y=12（24px，行高 27），字段从 68 起、间隔 28。 */
    const int nick_y = 12;
    const int field_y[3] = {68, 96, 124};
    const int field_line_h = (int)passport_ui_font_size(14)->line_height; /* 14px 行高 = 16 */
    const int fields_bottom = field_y[2] + field_line_h;                  /* 140 */

    /* Left: avatar image，上边缘与昵称字段上缘对齐，下边界与学号字段下边界齐平；
     * 背景色 = 主页背景（t->background），不再用深灰 */
    int img_w = 102, img_h = fields_bottom;
    int img_x = 0, img_y = nick_y;

    lv_obj_t *img_placeholder = lv_obj_create(s_home_content);
    lv_obj_set_size(img_placeholder, img_w, img_h);
    lv_obj_set_pos(img_placeholder, img_x, img_y);
    lv_obj_set_style_bg_color(img_placeholder, lv_color_hex(t->background), 0); /* 与主页背景一致 */
    lv_obj_set_style_bg_opa(img_placeholder, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(img_placeholder, 0, 0);
    lv_obj_set_style_radius(img_placeholder, 4, 0);
    lv_obj_set_style_clip_corner(img_placeholder, true, 0);

    /* 头像模式：设置里的"动态头像"开关(NVS bool dyn, 默认开)=动态精灵；关=静态照片
     * avatar.raw。关但照片缺失/尺寸不符 → 回落动态，避免空白。 */
    bool pet_mode = nvs_get_bool("dyn", true);

    free(s_avatar_dsc); s_avatar_dsc = NULL;
    free(s_avatar_buf); s_avatar_buf = NULL;
    if (pet_mode) {
        human_display_start(&(human_display_cfg_t){
            .parent = img_placeholder, .box_w = img_w, .box_h = img_h,
        });
    } else {
    FILE *f = fopen(PASSPORT_FS_ROOT "/avatar.raw", "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        size_t fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        size_t expected = (size_t)img_w * img_h * 2;
        if (fsize == expected) {
            s_avatar_buf = malloc(expected);
            s_avatar_dsc = malloc(sizeof(lv_image_dsc_t));
            if (s_avatar_buf && s_avatar_dsc) {
                fread(s_avatar_buf, 1, expected, f);
                *s_avatar_dsc = (lv_image_dsc_t){
                    .header.magic = LV_IMAGE_HEADER_MAGIC,
                    .header.cf = LV_COLOR_FORMAT_RGB565,
                    .header.w = img_w,
                    .header.h = img_h,
                    .header.stride = img_w * 2,
                    .data_size = expected,
                    .data = s_avatar_buf,
                };
                lv_obj_set_style_bg_image_src(img_placeholder, s_avatar_dsc, 0);
                lv_obj_set_style_bg_image_opa(img_placeholder, LV_OPA_COVER, 0);
            }
        }
        fclose(f);
    }
    if (!s_avatar_dsc) {
        /* 想显示静态照片但文件缺失/尺寸不符 → 回落动态精灵，不留空白 */
        human_display_start(&(human_display_cfg_t){
            .parent = img_placeholder, .box_w = img_w, .box_h = img_h,
        });
    }
    } /* photo 模式结束 */

    /* Right: info fields，文本列右对齐、右侧留 3px（贴死边缘会切掉字形最后一笔） */
    int info_x = img_x + img_w;           /* 102：头像右缘 = 文本列左缘，无空隙 */
    int info_w = BSP_LCD_W - info_x - 3;  /* 135px：容纳「学院:马克思主义学院」（半角冒号） */

    static const char *field_names[] = {"nickname", "college", "major", "student_id"};
    static const char *field_deflabels[] = {NULL, "学院", "专业", "学号"}; /* 标签可被 {名}_label.txt 覆盖 */
    static const char *field_defaults[] = {"o-Platform", "", "", ""};
    uint32_t field_colors[] = {0xFFFFFF, 0xCCCCCC, 0xCCCCCC, 0x888888};
    lv_obj_t **field_ptrs[] = {&s_home_nickname, &s_home_college, &s_home_major, &s_home_student_id};

    int vis_secondary = 0;   /* 已显示的第二类行数（用于紧凑排布，空行跳过） */
    for (int i = 0; i < 4; i++) {
        char path[64];
        char buf[64] = {0};
        char value[64] = {0};
        char text[128];
        size_t len = 0;

        snprintf(path, sizeof(path), PASSPORT_FS_ROOT "/%s.txt", field_names[i]);
        if (passport_storage_read_text(path, buf, sizeof(buf), &len) == ESP_OK && len > 0) {
            char *sv = buf;
            while (*sv == '\r' || *sv == '\n' || *sv == ' ' || *sv == '\t') sv++;
            size_t sl = strlen(sv);
            while (sl > 0 && (sv[sl - 1] == '\r' || sv[sl - 1] == '\n' || sv[sl - 1] == ' ' || sv[sl - 1] == '\t'))
                sv[--sl] = 0;
            snprintf(value, sizeof(value), "%s", sv);
        }

        /* 昵称为空用默认；其余三行值为空 → 整行不显示 */
        if (i == 0 && !value[0]) snprintf(value, sizeof(value), "%s", field_defaults[0]);
        if (i != 0 && !value[0]) { *field_ptrs[i] = NULL; continue; }

        /* 标签(第二类行可自定义 {名}_label.txt；无则用默认；昵称无标签) */
        char label[24] = {0};
        if (i != 0) {
            char lp[64]; size_t ll = 0; char lb[24] = {0};
            snprintf(lp, sizeof(lp), PASSPORT_FS_ROOT "/%s_label.txt", field_names[i]);
            if (passport_storage_read_text(lp, lb, sizeof(lb), &ll) == ESP_OK && ll > 0) {
                char *p = lb; while (*p=='\r'||*p=='\n') p++;
                size_t n = strlen(p); while (n>0 && (p[n-1]=='\r'||p[n-1]=='\n')) p[--n]=0;
                if (p[0]) snprintf(label, sizeof(label), "%s", p);
                else if (field_deflabels[i]) snprintf(label, sizeof(label), "%s", field_deflabels[i]);
            } else if (field_deflabels[i]) {
                snprintf(label, sizeof(label), "%s", field_deflabels[i]);
            }
        }
        if (i == 0) snprintf(text, sizeof(text), "%s", value);
        else if (label[0]) snprintf(text, sizeof(text), "%s:%s", label, value);
        else snprintf(text, sizeof(text), "%s", value);

        /* colour */
        uint32_t color = field_colors[i];
        snprintf(path, sizeof(path), PASSPORT_FS_ROOT "/%s_color.txt", field_names[i]);
        if (passport_storage_read_text(path, buf, sizeof(buf), &len) == ESP_OK && len >= 6) {
            buf[6] = 0; unsigned int c = 0;
            if (sscanf(buf, "%x", &c) == 1) color = c;
        }

        /* size (14/24 两档; 昵称固定 24) */
        int font_size = 14;
        snprintf(path, sizeof(path), PASSPORT_FS_ROOT "/%s_sz.txt", field_names[i]);
        if (passport_storage_read_text(path, buf, sizeof(buf), &len) == ESP_OK && len > 0) {
            int parsed = 0; if (sscanf(buf, "%d", &parsed) == 1) font_size = parsed;
        }
        if (i == 0) font_size = 24;
        const lv_font_t *field_font = passport_ui_font_size(font_size);

        /* bold (合成) */
        bool bold = false;
        snprintf(path, sizeof(path), PASSPORT_FS_ROOT "/%s_bold.txt", field_names[i]);
        if (passport_storage_read_text(path, buf, sizeof(buf), &len) == ESP_OK && len > 0)
            bold = (buf[0] == '1');

        int y;
        lv_text_align_t align;
        if (i == 0) { y = nick_y; align = LV_TEXT_ALIGN_CENTER; }
        else {
            y = field_y[0] + vis_secondary * 28;   /* 从 68 起, 紧凑排, 空行不占位 */
            vis_secondary++;
            align = LV_TEXT_ALIGN_LEFT;
        }

        if (bold) {
            lv_obj_t *shadow = lv_label_create(s_home_content);
            lv_obj_set_pos(shadow, info_x + 1, img_y + y);
            lv_obj_set_width(shadow, info_w);
            lv_obj_set_style_text_color(shadow, lv_color_hex(color), 0);
            lv_obj_set_style_text_font(shadow, field_font, 0);
            lv_obj_set_style_text_align(shadow, align, 0);
            lv_label_set_long_mode(shadow, LV_LABEL_LONG_DOT);
            lv_label_set_text(shadow, text);
        }
        lv_obj_t *label_obj = lv_label_create(s_home_content);
        lv_obj_set_pos(label_obj, info_x, img_y + y);
        lv_obj_set_width(label_obj, info_w);
        lv_obj_set_style_text_color(label_obj, lv_color_hex(color), 0);
        lv_obj_set_style_text_font(label_obj, field_font, 0);
        lv_obj_set_style_text_align(label_obj, align, 0);
        lv_label_set_long_mode(label_obj, LV_LABEL_LONG_DOT);
        lv_label_set_text(label_obj, text);
        *field_ptrs[i] = label_obj;
    }

    /* === Bottom bar: 参照设置页 key_bar（surface 底 + 顶部分割线），按钮叠于其上 === */
    lv_obj_t *bottom_bar = lv_obj_create(s_home_screen);
    lv_obj_remove_flag(bottom_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bottom_bar, BSP_LCD_W, 72);
    lv_obj_set_pos(bottom_bar, 0, BSP_LCD_H - 72);
    lv_obj_set_style_bg_color(bottom_bar, lv_color_hex(t->surface), 0);
    lv_obj_set_style_bg_opa(bottom_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bottom_bar, 1, 0);
    lv_obj_set_style_border_side(bottom_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(bottom_bar, lv_color_hex(t->divider), 0);
    lv_obj_set_style_radius(bottom_bar, 0, 0);
    lv_obj_set_style_pad_all(bottom_bar, 0, 0);

    /* === Bottom buttons: side by side, equal width, with margin === */
    int btn_margin = 10;
    int btn_gap = 10;
    int btn_w = (BSP_LCD_W - 2 * btn_margin - btn_gap) / 2;
    int btn_h = 44;
    int btn_y = BSP_LCD_H - btn_h - 14;

    s_home_btn_apps = lv_obj_create(s_home_screen);
    lv_obj_remove_flag(s_home_btn_apps, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_home_btn_apps, btn_w, btn_h);
    lv_obj_set_pos(s_home_btn_apps, btn_margin, btn_y);
    lv_obj_set_style_bg_color(s_home_btn_apps, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_home_btn_apps, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_home_btn_apps, lv_color_hex(t->accent), 0);
    lv_obj_set_style_border_width(s_home_btn_apps, 1, 0);
    lv_obj_set_style_radius(s_home_btn_apps, 8, 0);
    lv_obj_t *picon = lv_label_create(s_home_btn_apps);
    lv_obj_set_style_text_color(picon, lv_color_hex(t->accent), 0);
    lv_obj_set_style_text_font(picon, passport_ui_font_size(24), 0);
    lv_obj_align(picon, LV_ALIGN_LEFT_MID, 10, 0);
    lv_label_set_text(picon, "\xEF\x83\x8A");
    lv_obj_t *ptext = lv_label_create(s_home_btn_apps);
    lv_obj_set_style_text_color(ptext, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(ptext, passport_ui_font_size(14), 0);
    lv_obj_align(ptext, LV_ALIGN_LEFT_MID, 36, 0);
    lv_label_set_text(ptext, "应用");

    s_home_btn_settings = lv_obj_create(s_home_screen);
    lv_obj_remove_flag(s_home_btn_settings, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_home_btn_settings, btn_w, btn_h);
    lv_obj_set_pos(s_home_btn_settings, btn_margin + btn_w + btn_gap, btn_y);
    lv_obj_set_style_bg_color(s_home_btn_settings, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_home_btn_settings, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_home_btn_settings, lv_color_hex(t->accent), 0);
    lv_obj_set_style_border_width(s_home_btn_settings, 1, 0);
    lv_obj_set_style_radius(s_home_btn_settings, 8, 0);
    lv_obj_t *sicon = lv_label_create(s_home_btn_settings);
    lv_obj_set_style_text_color(sicon, lv_color_hex(t->accent), 0);
    lv_obj_set_style_text_font(sicon, passport_ui_font_size(24), 0);
    lv_obj_align(sicon, LV_ALIGN_LEFT_MID, 10, 0);
    lv_label_set_text(sicon, "\xEF\x80\x93");
    lv_obj_t *stext = lv_label_create(s_home_btn_settings);
    lv_obj_set_style_text_color(stext, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(stext, passport_ui_font_size(14), 0);
    lv_obj_align(stext, LV_ALIGN_LEFT_MID, 36, 0);
    lv_label_set_text(stext, "设置");

    /* Initial selection: first button */
    s_home_selected = 0;
    lv_obj_set_style_bg_color(s_home_btn_apps, lv_color_hex(t->accent), 0);
    lv_obj_set_style_bg_opa(s_home_btn_apps, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(picon, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_color(ptext, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_color(s_home_btn_settings, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_color(sicon, lv_color_hex(t->accent), 0);
    lv_obj_set_style_text_color(stext, lv_color_hex(0xFFFFFF), 0);

    lv_screen_load(s_home_screen);
    s_view = VIEW_LAUNCHER;

    s_home_timer = lv_timer_create(update_home_status, 5000, NULL);
    update_home_status(s_home_timer);
    lv_timer_set_auto_delete(s_home_timer, false);
}

static void destroy_native_view(void)
{
    s_on_network_page = false;

    /* 离开配网页即停配网服务:任务在跑则置 cancel,让 wifi_prov_task 退出并关热点。
     * (配网成功/失败后任务已自行退出,s_wifi_task_alive=false,不会误触) */
    if (s_wifi_task_alive) s_wifi_cancel = true;

    /* 离开语音/PPT 页即关蓝牙(退则关):两处开关之一。BLE 由 show_voice/show_ppt
     * 进入时启动,离开后不再常驻。仅当确实从语音/PPT 页切走时触发(s_view 此刻还是旧页面)。 */
    if (s_view == VIEW_VOICE || s_view == VIEW_PPT) {
        if (ble_prov_is_running()) ble_stack_stop();
        s_bt_enabled = false;
        nvs_set_bool("bt", false);
    }

    /* WiFi 按需启用/退出即关:离开需联网页面(传输/无线设置)即释放内存。
     * wifi_sta_stop() 在 WiFi 未启用时是空操作,故仅在确实从这两页切走时调用,
     * 不影响配网热点(softAP 由 wifi_prov_task 独立管理)。 */
    if (s_view == VIEW_TRANSFER || s_view == VIEW_WIFI) {
        wifi_sta_stop();
    }

    /* 配网页的轮询定时器必须在页面销毁时一并停掉，
     * 否则回调会继续写已释放的 s_wifi_status/s_wifi_hint。 */
    if (s_wifi_poll) {
        lv_timer_del(s_wifi_poll);
        s_wifi_poll = NULL;
    }
    s_wifi_status = NULL;
    s_wifi_hint = NULL;
    voice_stop_timer();
    s_voice_hint = NULL;
    s_voice_text = NULL;
    s_voice_timer_label = NULL;
    ppt_stop_timer();
    s_ppt_hint = NULL;
    s_ppt_action = NULL;
    s_ppt_timer_label = NULL;
    if (s_view == VIEW_TRANSFER) destroy_transfer_page();
    if (s_list) {
        passport_ui_list_destroy(s_list);
        s_list = NULL;
    }
    if (s_page) {
        passport_ui_page_destroy(s_page);
        s_page = NULL;
    }
}
static void format_setting_value(passport_setting_id_t id,
                                 uint16_t value,
                                 char *out,
                                 size_t capacity)
{
    if (id == PASSPORT_SETTING_BRIGHTNESS || id == PASSPORT_SETTING_VOLUME) {
        snprintf(out, capacity, "%u%%", (unsigned)value);
    } else if (id == PASSPORT_SETTING_KEY_SOUND) {
        snprintf(out, capacity, "%s", value ? "开启" : "关闭");
    } else if (value == 0U) {
        snprintf(out, capacity, "从不");
    } else if (value < 60U) {
        snprintf(out, capacity, "%u 秒", (unsigned)value);
    } else {
        snprintf(out, capacity, "%u 分钟", (unsigned)(value / 60U));
    }
}

static void refresh_settings(void)
{
    if (!s_page || !s_list) return;
    for (size_t i = 0; i < SETTINGS_VALUE_ROW_COUNT; ++i) {
        uint16_t value = 0U;
        char text[24];
        if (!passport_settings_get(SETTINGS_ROWS[i], &value)) continue;
        format_setting_value(SETTINGS_ROWS[i], value, text, sizeof(text));
        size_t list_idx = i + 1;
        if (s_settings_editing && list_idx == passport_ui_list_selected(s_list)) {
            strcat(text, " <");
        }
        passport_ui_list_set_value(s_list, list_idx, text);
    }
    /* 动态头像开关行（值行之后第一行），显示当前 开/关 */
    passport_ui_list_set_value(s_list, SETTINGS_VALUE_ROW_COUNT + 1,
                               nvs_get_bool("dyn", true) ? "开" : "关");
    const size_t selected = passport_ui_list_selected(s_list);
    const char *action = "调整";
    if (s_settings_editing) action = "确定";
    else if (selected == 0 || selected >= SETTINGS_VALUE_ROW_COUNT + 1) action = "查看";
    passport_ui_page_set_actions(s_page, action, "主页");
}

static void show_settings(void)
{
    destroy_native_view();
    s_prev_view = s_view;
    s_settings_editing = false;
    s_page = passport_ui_page_create("设置", true, true);
    s_list = passport_ui_list_create(s_page, SETTINGS_LIST_ROW_COUNT);
    passport_ui_list_add(s_list, "网络与连接");
    for (size_t i = 0; i < SETTINGS_VALUE_ROW_COUNT; ++i) {
        passport_ui_list_add_value(s_list, SETTINGS_NAMES[i], "");
    }
    passport_ui_list_add_value(s_list, "动态头像", "");
    passport_ui_list_add(s_list, "设备信息");
    passport_ui_list_add(s_list, "主题");
    refresh_settings();
    passport_ui_page_show(s_page);
    s_view = VIEW_SETTINGS;
}

static void show_device_info(void)
{
    destroy_native_view();
    s_prev_view = s_view;
    s_page = passport_ui_page_create("设备信息", true, true);
    char line[300];
    unsigned long app_total = 0, data_total = 0;
    const char *app_label = "", *data_label = "";

    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    if (p) { app_total = p->size; app_label = p->label; }
    p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    if (p) { data_total = p->size; data_label = p->label; }

    unsigned long app_used = app_total - 0x1ED7D0;
    snprintf(line, sizeof(line), "固件(%s):  %.2fMB / %.2fMB", app_label, app_used / 1048576.0, app_total / 1048576.0);
    passport_ui_label_create(s_page, line);
    snprintf(line, sizeof(line), "数据(%s):  - / %.2fMB", data_label, data_total / 1048576.0);
    passport_ui_label_create(s_page, line);
    passport_ui_label_create(s_page, "");
    unsigned long total_used = 24 + 4 + app_used / 1024;
    snprintf(line, sizeof(line), "已用:  %luMB / 8MB", (total_used + 512) / 1024);
    passport_ui_label_create(s_page, line);
    passport_ui_label_create(s_page, "");
    snprintf(line, sizeof(line), "设备码: %s", passport_identity_code());
    passport_ui_label_create(s_page, line);
    passport_ui_page_set_actions(s_page, "返回", "主页");
    passport_ui_page_show(s_page);
    s_view = VIEW_DEVICE_INFO;
}

static void show_themes(void)
{
    destroy_native_view();
    s_prev_view = s_view;
    s_theme_count = passport_theme_list(s_themes, PASSPORT_MAX_INSTALLED_THEMES);
    s_page = passport_ui_page_create("主题", true, true);
    s_list = passport_ui_list_create(s_page, PASSPORT_MAX_INSTALLED_THEMES);
    for (size_t i = 0; i < s_theme_count; ++i) {
        char row[80];
        snprintf(row, sizeof(row), "%s%s", s_themes[i].name,
                 strcmp(s_themes[i].id, passport_theme_current_id()) == 0 ? "  当前" : "");
        passport_ui_list_add(s_list, row);
    }
    passport_ui_page_set_actions(s_page, "应用", "主页");
    passport_ui_page_show(s_page);
    s_view = VIEW_THEMES;
}

static void wifi_prov_task(void *arg)
{
    (void)arg;
    esp_task_wdt_delete(NULL);
    wifi_sta_set_suspended(true);

    /* BLE guard:从语音页进配网时 BLE 可能开着(语音通道),ESP32-C3 无 PSRAM,
     * 常驻 BLE + APSTA 会 panic,故配网前停栈腾堆。配网结束不恢复 BLE——
     * BLE 只由语音页按需启动,离开语音页即停,进语音页再自行启动。 */
    if (ble_prov_is_running()) {
        ble_stack_stop();
    }

    esp_err_t err = wifi_ap_prov_start();
    if (err != ESP_OK) {
        wifi_sta_set_suspended(false);
        s_wifi_phase = 4;
        s_wifi_task_alive = false;
        vTaskDelete(NULL);
        return;
    }

    char ssid[33] = {0}, pass[65] = {0};
    while (!s_wifi_cancel) {
        if (wifi_ap_prov_get_creds(ssid, sizeof(ssid), pass, sizeof(pass))) break;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    wifi_sta_set_suspended(false);
    wifi_ap_prov_stop();

    if (s_wifi_cancel) {
        s_wifi_phase = 4;
        s_wifi_task_alive = false;
        vTaskDelete(NULL);
        return;
    }

    s_wifi_phase = 1;
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_err_t rc = wifi_sta_connect(ssid, pass[0] ? pass : NULL);
    if (rc == ESP_OK) {
        s_wifi_phase = 1;
        esp_http_client_config_t cfg = { .url = "http://www.baidu.com/", .timeout_ms = 10000, .buffer_size = 1024 };
        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        if (cli) {
            esp_err_t hr = esp_http_client_perform(cli);
            int st = esp_http_client_get_status_code(cli);
            esp_http_client_cleanup(cli);
            s_wifi_phase = (hr == ESP_OK && st == 200) ? 2 : 3;
        } else { s_wifi_phase = 3; }
    } else { s_wifi_phase = 4; }
    s_wifi_task_alive = false;
    vTaskDelete(NULL);
}

static void wifi_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_wifi_status) return;
    switch (s_wifi_phase) {
    case 0: lv_label_set_text(s_wifi_status, "热点已开启\n等待手机配置..."); break;
    case 1: lv_label_set_text(s_wifi_status, "收到凭证\n正在连接..."); break;
    case 2: s_wifi_poll = NULL; lv_timer_del(t);
        lv_label_set_text(s_wifi_hint, "长按 OK 返回");
        lv_label_set_text(s_wifi_status, "配网成功!\n联网验证通过"); break;
    case 3: s_wifi_poll = NULL; lv_timer_del(t);
        lv_label_set_text(s_wifi_hint, "长按 OK 返回");
        lv_label_set_text(s_wifi_status, "已连上 WiFi\n但无法上网"); break;
    case 4: s_wifi_poll = NULL; lv_timer_del(t);
        lv_label_set_text(s_wifi_hint, "按 OK 重试 / 长按 OK 返回");
        lv_label_set_text(s_wifi_status, "配网失败或已取消"); break;
    }
}

static void show_wifi_prov(void)
{
    s_wifi_view = WIFI_PROV;
    s_wifi_phase = 0;
    s_wifi_cancel = false;
    destroy_native_view();
    /* 配网依赖 WiFi 驱动(softAP 复用同一驱动):校时后 WiFi 可能已 deinit,进入先 init。
     * 配网是建立 WiFi 的手段,不随 s_wifi_enabled 关闭 —— 保证用户总能重新配网。 */
    wifi_sta_init();
    s_page = passport_ui_page_create("热点配网", true, true);
    passport_ui_page_set_actions(s_page, "", "取消");

    if (s_wifi_task_alive) {
        s_wifi_status = passport_ui_label_create(s_page, "上一轮配网还在进行");
        s_wifi_hint = passport_ui_label_create(s_page, "长按 OK 返回");
        passport_ui_page_show(s_page);
        return;
    }

    /* BLE 不在配网前由这里停:配网前停栈兜底已放在 wifi_prov_task 内
     * (从语音页进配网时 BLE 可能开着,需腾堆)。配网结束也不恢复 BLE,
     * BLE 只由语音页按需启动/关闭。 */
    s_wifi_status = passport_ui_label_create(s_page, "热点已开启\n等待手机配置...");
    s_wifi_hint = passport_ui_label_create(s_page, "长按 OK 取消");
    passport_ui_page_show(s_page);

    s_wifi_poll = lv_timer_create(wifi_poll_cb, 500, NULL);
    s_wifi_task_alive = true;
    /* 栈 8192:WifiConfigurationAp::StartAccessPoint 有 ota_url[256] 等
     * 大局部变量,HTTP/DNS server 启动也在本任务栈上,4096 会溢出。 */
    if (xTaskCreate(wifi_prov_task, "wifi_prov", 8192, NULL, 3, NULL) != pdPASS) {
        lv_label_set_text(s_wifi_status, "内存不足");
        if (s_wifi_hint) lv_label_set_text(s_wifi_hint, "按 OK 重试");
        s_wifi_phase = 4;
        s_wifi_task_alive = false;
        /* 任务没起来:若 BLE 在跑(从语音页进配网被 wifi_prov_task 停掉的兜底
         * 没执行到),保持关;用户回语音页会由 show_voice 重新启动。 */
    }
}

static void wifi_connect_task(void *arg)
{
    char **pair = (char **)arg;
    char *ssid = pair[0];
    char *pass = pair[1];
    char *prev_ssid = pair[2];
    bool had_prev = prev_ssid && prev_ssid[0];
    free(pair);
    esp_task_wdt_delete(NULL);
    esp_err_t rc = wifi_sta_connect(ssid, pass);
    if (rc != ESP_OK && had_prev) {
        char prev_pass[65] = {0};
        wifi_sta_get_saved_creds(prev_ssid, prev_pass, sizeof(prev_pass));
        wifi_sta_connect(prev_ssid, prev_pass);
    }
    free(ssid);
    if (pass) free(pass);
    if (prev_ssid) free(prev_ssid);
    system_event_t ev = { .type = EVENT_WIFI_CONNECT_DONE };
    ev.data.wifi_connect.ok = (rc == ESP_OK);
    xQueueSend(s_events, &ev, 0);
    vTaskDelete(NULL);
}

static void show_network(void)
{
    destroy_native_view();
    s_prev_view = s_view;
    s_on_network_page = true;
    s_page = passport_ui_page_create("网络与连接", true, true);
    s_list = passport_ui_list_create(s_page, 4);
    passport_ui_list_add_value(s_list, "蓝牙", s_bt_enabled ? "开" : "关");
    passport_ui_list_add_value(s_list, "WiFi", s_wifi_enabled ? "开" : "关");
    passport_ui_list_add(s_list, "WiFi 配网");
    passport_ui_list_add(s_list, "传输");
    passport_ui_page_set_actions(s_page, "切换", "主页");
    passport_ui_page_show(s_page);
    s_view = VIEW_DEVICE_INFO;
}

static void show_wifi(void)
{
    s_wifi_view = WIFI_INFO;
    destroy_native_view();
    s_prev_view = s_view;
    /* 无线设置需查询/连接 WiFi:按需启用,离开由 destroy_native_view 停 */
    if (s_wifi_enabled) {
        if (wifi_sta_init() == ESP_OK) wifi_sta_connect_default();
    }
    s_page = passport_ui_page_create("无线设置", true, true);

    char saved_ssids[WIFI_MAX_SAVED][33];
    int saved_count = wifi_sta_list_saved(saved_ssids, WIFI_MAX_SAVED);

    if (wifi_sta_is_connected()) {
        s_list = passport_ui_list_create(s_page, 1 + saved_count);
        s_wifi_status = passport_ui_label_create(s_page, "已连接");
        char line[300];
        snprintf(line, sizeof(line), "当前: %s", wifi_sta_current_ssid());
        passport_ui_list_add(s_list, line);
        for (int i = 0; i < saved_count; i++) {
            if (strcmp(saved_ssids[i], wifi_sta_current_ssid()) != 0) {
                snprintf(line, sizeof(line), "切换到 %s", saved_ssids[i]);
                passport_ui_list_add(s_list, line);
            }
        }
        passport_ui_list_add(s_list, "断开连接");
        s_wifi_hint = passport_ui_label_create(s_page, "");
        passport_ui_page_set_actions(s_page, "切换", "主页");
    } else if (saved_count > 0) {
        s_list = passport_ui_list_create(s_page, saved_count + 1);
        s_wifi_status = passport_ui_label_create(s_page, "");
        char line[300];
        for (int i = 0; i < saved_count; i++) {
            snprintf(line, sizeof(line), "%s", saved_ssids[i]);
            passport_ui_list_add(s_list, line);
        }
        passport_ui_list_add(s_list, "热点配网");
        s_wifi_hint = passport_ui_label_create(s_page, "按 OK 连接选中网络");
        passport_ui_page_set_actions(s_page, "连接", "配网");
    } else {
        passport_ui_label_create(s_page, "未连接 WiFi\n\n按 OK 开始热点配网");
        passport_ui_page_set_actions(s_page, "配网", "主页");
    }
    passport_ui_page_show(s_page);
    s_view = VIEW_WIFI;
}

/* Handle WiFi page key events - called from handle_settings_key or handle_device_info_key */
/* Actually, we need to add WiFi key handling. Let me keep it simple and use the existing view. */

static void handle_launcher_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    const passport_theme_tokens_t *t = passport_theme_current();
    const int delta = passport_input_navigation_delta(btn, ev);
    if (delta != 0) {
        s_home_selected = (s_home_selected + delta) & 1;
        lv_obj_t *btns[2] = {s_home_btn_apps, s_home_btn_settings};
        lv_obj_t *icons[2], *texts[2];
        texts[0] = lv_obj_get_child(s_home_btn_apps, 1);
        icons[0] = lv_obj_get_child(s_home_btn_apps, 0);
        texts[1] = lv_obj_get_child(s_home_btn_settings, 1);
        icons[1] = lv_obj_get_child(s_home_btn_settings, 0);
        for (int i = 0; i < 2; ++i) {
            if (i == s_home_selected) {
                lv_obj_set_style_bg_color(btns[i], lv_color_hex(t->accent), 0);
                lv_obj_set_style_bg_opa(btns[i], LV_OPA_COVER, 0);
                lv_obj_set_style_text_color(icons[i], lv_color_hex(0x000000), 0);
                lv_obj_set_style_text_color(texts[i], lv_color_hex(0x000000), 0);
            } else {
                lv_obj_set_style_bg_color(btns[i], lv_color_hex(0x000000), 0);
                lv_obj_set_style_text_color(icons[i], lv_color_hex(t->accent), 0);
                lv_obj_set_style_text_color(texts[i], lv_color_hex(0xFFFFFF), 0);
            }
        }
    } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        if (s_home_selected == 0) show_apps();
        else if (s_home_selected == 1) show_settings();
    }
}

static void handle_apps_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s_list || builtin_app_count() == 0) return;
    const int delta = passport_input_navigation_delta(btn, ev);
    if (delta != 0) passport_ui_list_move(s_list, delta);
    else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        size_t selected = passport_ui_list_selected(s_list);
        if (selected < (size_t)builtin_app_count()) BUILTIN_APPS[selected].open();
    }
}

static void handle_themes_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s_list || s_theme_count == 0) return;
    const int delta = passport_input_navigation_delta(btn, ev);
    if (delta != 0) passport_ui_list_move(s_list, delta);
    else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        size_t selected = passport_ui_list_selected(s_list);
        if (selected < s_theme_count && passport_theme_apply(s_themes[selected].id) == ESP_OK) show_themes();
    }
}

static void handle_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s_list) return;
    const int delta = passport_input_navigation_delta(btn, ev);
    const size_t selected = passport_ui_list_selected(s_list);

    if (delta != 0) {
        if (s_settings_editing && selected >= 1 && selected < SETTINGS_VALUE_ROW_COUNT + 1) {
            size_t row = selected - 1;
            passport_settings_cycle(SETTINGS_ROWS[row], -delta);
            if (SETTINGS_ROWS[row] == PASSPORT_SETTING_VOLUME) {
                passport_settings_sound_preview();
            }
        } else {
            passport_ui_list_move(s_list, delta);
        }
        refresh_settings();
        return;
    }
    if (btn != BSP_BTN_OK || ev != BSP_BTN_CLICK) return;
    if (selected == 0) { show_network(); return; }
    /* 动态头像开关行：OK 直接翻转，主页即时生效 */
    if (selected == SETTINGS_VALUE_ROW_COUNT + 1) {
        nvs_set_bool("dyn", !nvs_get_bool("dyn", true));
        refresh_settings();
        return;
    }
    if (selected == SETTINGS_VALUE_ROW_COUNT + 2) { show_device_info(); return; }
    if (selected == SETTINGS_VALUE_ROW_COUNT + 3) { show_themes(); return; }
    if (selected < 1 || selected >= SETTINGS_VALUE_ROW_COUNT + 1) return;

    s_settings_editing = !s_settings_editing;
    refresh_settings();
}

static void show_transfer_page(void)
{
    destroy_native_view();
    destroy_transfer_page();
    s_prev_view = s_view;
    /* 传输走 WiFi STA(HTTP server 需 IP):按需启用,离开由 destroy_native_view 停 */
    if (s_wifi_enabled) {
        if (wifi_sta_init() == ESP_OK) wifi_sta_connect_default();
    }
    show_transfer();
    s_view = VIEW_TRANSFER;
}

static void handle_transfer_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        show_transfer_wifi();
    }
}

static void destroy_transfer_page(void)
{
    transfer_stop();
}

static void handle_device_info_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) show_settings();
}

static bool consume_screen_wake(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (s_screen_off) {
        if (ev == BSP_BTN_PRESS) {
            s_screen_off = false;
            bsp_display_backlight(100);
            human_display_resume();
        }
        return true;
    }
    const bool woke = passport_settings_note_activity();
    const bool terminal = ev == BSP_BTN_CLICK || ev == BSP_BTN_DOUBLE ||
                          ev == BSP_BTN_LONG;
    return passport_settings_model_consume_wake(
        &s_wake_guard, (uint8_t)btn, ev == BSP_BTN_PRESS, terminal, woke);
}

static bool is_key_sound_event(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    return ev == BSP_BTN_CLICK || ev == BSP_BTN_DOUBLE ||
           (btn == BSP_BTN_OK && ev == BSP_BTN_LONG);
}

/* ---------------- 语音输入(PTT 按住 ↓ 说话,应用菜单条目) ---------------- */

static void voice_set_hint(const char *text)
{
    if (s_voice_hint) lv_label_set_text(s_voice_hint, text);
}

static void voice_stop_timer(void)
{
    if (s_voice_timer) {
        lv_timer_del(s_voice_timer);
        s_voice_timer = NULL;
    }
    if (s_voice_vu_timer) {
        lv_timer_del(s_voice_vu_timer);
        s_voice_vu_timer = NULL;
    }
}

/* 就绪态提示按优先级:BLE 未启动(异常) > 桌面端未连 > 就绪 */
static bool s_voice_link_logged;
static void voice_refresh_hint(void)
{
    if (!ble_prov_is_running()) {
        voice_set_hint("蓝牙未启动\n请重试或到 网络与连接 打开");
        s_voice_link_logged = false;
    } else if (!passport_voice_link_ready()) {
        voice_set_hint("桌面端未连接\n\n按住 ↓ 说话");
        if (!s_voice_link_logged) {
            ESP_LOGE(TAG, "桌面端未连接(等待 BLE 连接)");
            s_voice_link_logged = true;
        }
    } else {
        voice_set_hint("按住 ↓ 说话\n\n松开发送");
        s_voice_link_logged = false;
        voice_set_hint("按住 ↓ 说话\n\n松开发送");
    }
}

static void voice_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_voice_timer_label || s_voice_state != VOICE_RECORDING) return;
    uint32_t ms = (xTaskGetTickCount() - s_voice_start_tick) * portTICK_PERIOD_MS;
    char text[24];
    snprintf(text, sizeof(text), "%u.%u s",
             (unsigned)(ms / 1000), (unsigned)(ms % 1000) / 100);
    lv_label_set_text(s_voice_timer_label, text);
}

static void voice_start_recording(void)
{
    if (passport_voice_start_session() != ESP_OK) {
        passport_voice_play_tone(PASSPORT_VOICE_TONE_ERROR);
        voice_set_hint("录音启动失败\n\n按住 ↓ 重试");
        return;
    }
    s_voice_state = VOICE_RECORDING;
    s_voice_start_tick = xTaskGetTickCount();
    voice_set_hint("已开始录音");
    lv_obj_set_style_text_color(s_voice_hint, lv_color_hex(0x00FF00), 0);
    if (s_voice_timer_label) lv_label_set_text(s_voice_timer_label, "0.0 s");
    if (!s_voice_timer) s_voice_timer = lv_timer_create(voice_timer_cb, 500, NULL);
    if (!s_voice_vu_timer) s_voice_vu_timer = lv_timer_create(voice_vu_cb, 100, NULL);
    if (s_voice_bar_bg) lv_obj_remove_flag(s_voice_bar_bg, LV_OBJ_FLAG_HIDDEN);
}

/* send=false:当场收束丢弃(误触/断链/退出),不发任何边界帧 */
static void voice_finish_recording(bool send)
{
    voice_stop_timer();
    if (send) {
        uint32_t drops = 0;
        passport_voice_stop_session(&drops);
        ESP_LOGI(TAG, "语音会话结束,丢帧 %u", (unsigned)drops);
    } else {
        passport_voice_cancel_session();
    }
    s_voice_state = VOICE_READY;
    if (s_voice_timer_label) lv_label_set_text(s_voice_timer_label, "");
    voice_refresh_hint();
    if (s_voice_bar_bg) lv_obj_add_flag(s_voice_bar_bg, LV_OBJ_FLAG_HIDDEN);
}

static void handle_voice_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        show_home();
        return;
    }
    if (btn != BSP_BTN_DOWN) return;   /* UP 长按=返回上一层(全局),本页面不消费 */
    switch (s_voice_state) {
    case VOICE_TRANSCRIBING: break;
    case VOICE_READY:
        if (ev == BSP_BTN_PRESS) {
            if (!ble_prov_is_running() || !passport_voice_link_ready()) {
                passport_voice_play_tone(PASSPORT_VOICE_TONE_ERROR);
                voice_refresh_hint();
                return;
            }
            voice_start_recording();
        }
        break;
    case VOICE_RECORDING:
        if (ev == BSP_BTN_RELEASE) {
            uint32_t ms = (xTaskGetTickCount() - s_voice_start_tick) * portTICK_PERIOD_MS;
            voice_finish_recording(ms >= VOICE_MIN_TALK_MS);
        }
        break;
    
    }
}

static void voice_vu_cb(lv_timer_t *t)
{
    (void)t;
    if (s_voice_state != VOICE_RECORDING || !s_voice_bar_fg || !s_voice_bar_bg) return;
    uint16_t peak = passport_voice_peak();
    int w = lv_obj_get_width(s_voice_bar_bg);
    int bar_w = (int)((uint32_t)w * peak / 32768);
    if (bar_w < 2) bar_w = 2;
    if (bar_w > w) bar_w = w;
    lv_obj_set_width(s_voice_bar_fg, bar_w);
}

static void show_voice(void)
{
    destroy_native_view();
    /* 进入语音输入页即启用蓝牙(进则开):两处开关之一,无条件启动,
     * 保证语音随时可用;离开由 destroy_native_view 关闭(退则关)。 */
    /* BLE 与 WiFi 在无 PSRAM 的 C3 上互斥抢堆(与热点配网前停 BLE 同理,
     * 这里是反向:起 BLE 前先停 WiFi 腾堆)。开机校时若未完成,WiFi 常驻
     * ~99KB,NimBLE+esp_hid+语音 GATT 初始化会因堆不足失败 → 广播起不来、
     * 桌面端扫不到。故起 BLE 前强制释放 WiFi。 */
    wifi_sta_stop();
    s_bt_enabled = true;
    nvs_set_bool("bt", true);
    /* 语音页广播身份：名 "AI Passport Voice"、只广播 0xA2B0、外观=普通
     * 外设，避免 Windows 误判为 HID 键盘走已配对重连通道（通用扫描扫不到）。 */
    ble_prov_set_identity(true);
    if (!ble_prov_is_running()) {
        ble_prov_start();
    }
    s_prev_view = s_view;
    s_voice_state = VOICE_READY;
    s_page = passport_ui_page_create("语音输入", true, true);
    s_voice_hint = passport_ui_label_create(s_page, "");
    s_voice_text = passport_ui_label_create(s_page, "");
    s_voice_timer_label = passport_ui_label_create(s_page, "");

    lv_obj_t *content = passport_ui_page_content(s_page);
    s_voice_bar_bg = lv_obj_create(content);
    lv_obj_set_width(s_voice_bar_bg, BSP_LCD_W - 20);
    lv_obj_set_height(s_voice_bar_bg, 16);
    lv_obj_set_style_bg_color(s_voice_bar_bg, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(s_voice_bar_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_voice_bar_bg, 0, 0);
    lv_obj_set_style_radius(s_voice_bar_bg, 8, 0);
    lv_obj_set_style_pad_all(s_voice_bar_bg, 0, 0);
    lv_obj_add_flag(s_voice_bar_bg, LV_OBJ_FLAG_HIDDEN);

    s_voice_bar_fg = lv_obj_create(s_voice_bar_bg);
    lv_obj_set_height(s_voice_bar_fg, 16);
    lv_obj_set_width(s_voice_bar_fg, 2);
    lv_obj_set_style_bg_color(s_voice_bar_fg, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_bg_opa(s_voice_bar_fg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_voice_bar_fg, 0, 0);
    lv_obj_set_style_radius(s_voice_bar_fg, 8, 0);
    lv_obj_set_style_pad_all(s_voice_bar_fg, 0, 0);
    lv_obj_set_pos(s_voice_bar_fg, 0, 0);

    passport_ui_page_set_actions(s_page, "返回", "主页");
    passport_ui_page_show(s_page);
    voice_refresh_hint();
    s_view = VIEW_VOICE;
}

/* ---------------- PPT 遥控(蓝牙键盘:上一页/下一页/放映/退出) ---------------- */
static void ppt_stop_timer(void)
{
    if (s_ppt_timer) {
        lv_timer_del(s_ppt_timer);
        s_ppt_timer = NULL;
    }
}

static void ppt_refresh_status(void)
{
    if (!s_ppt_hint) return;
    if (!ble_prov_is_running()) {
        passport_ui_label_set_text(s_ppt_hint, "蓝牙未启动");
    } else if (passport_ppt_is_connected()) {
        passport_ui_label_set_text(s_ppt_hint, "已连接，可控制 PPT");
    } else if (passport_ppt_is_paired()) {
        passport_ui_label_set_text(s_ppt_hint, "已配对，等待连接…");
    } else {
        passport_ui_label_set_text(s_ppt_hint, "等待配对 / 连接…");
    }
}

static void ppt_set_action(const char *text, uint32_t color)
{
    if (!s_ppt_action) return;
    lv_label_set_text(s_ppt_action, text);
    lv_obj_set_style_text_color(s_ppt_action, lv_color_hex(color), 0);
}

static void ppt_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_ppt_timer_label || !s_ppt_hint) return;
    ppt_refresh_status();
    if (s_ppt_pres_running) {
        s_ppt_pres_seconds++;
        char buf[16];
        snprintf(buf, sizeof(buf), "%02u:%02u",
                 (unsigned)(s_ppt_pres_seconds / 60),
                 (unsigned)(s_ppt_pres_seconds % 60));
        lv_label_set_text(s_ppt_timer_label, buf);
    }
}

static void ppt_timer_reset_display(void)
{
    if (s_ppt_timer_label) lv_label_set_text(s_ppt_timer_label, "00:00");
}

static void handle_ppt_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    switch (btn) {
    case BSP_BTN_UP:
        if (ev == BSP_BTN_CLICK) {
            passport_ppt_key_press(0, PPT_HID_KEY_LEFT_ARROW);
            ppt_set_action("<< 上一页", 0x00A5FF);
        }
        break;
    case BSP_BTN_DOWN:
        if (ev == BSP_BTN_CLICK) {
            passport_ppt_key_press(0, PPT_HID_KEY_RIGHT_ARROW);
            ppt_set_action("下一页 >>", 0x00A5FF);
        } else if (ev == BSP_BTN_LONG) {
            /* 长按:Esc 退出放映 + 停止并重置计时。
             * (长按上键=返回上一层、长按 OK=返回主页 由全局导航处理,不进这里) */
            passport_ppt_key_press(0, PPT_HID_KEY_ESCAPE);
            s_ppt_pres_running = false;
            s_ppt_pres_seconds = 0;
            ppt_timer_reset_display();
            ppt_set_action("退出放映", 0xFF8800);
        }
        break;
    case BSP_BTN_OK:
        if (ev == BSP_BTN_CLICK) {
            /* 短按:开始放映(跨平台 F5/Cmd+Shift+Return/Opt+Cmd+P) */
            passport_ppt_press_start_slideshow();
            if (!s_ppt_pres_running) {
                s_ppt_pres_running = true;
                s_ppt_pres_seconds = 0;
                ppt_timer_reset_display();
            }
            ppt_set_action("开始放映", 0x00CC00);
        }
        /* 长按 OK 不再发 ESC:由 handle_key_event 的全局导航接管为「返回主页」,
         * 离开页面时 destroy_native_view 停 BLE,放映随之退出。 */
        break;
    default: break;
    }
}

static void show_ppt(void)
{
    destroy_native_view();
    /* 进入 PPT 遥控页即启用蓝牙(进则配对/退则停):与语音页共用同一 NimBLE 栈,
     * HID 键盘服务已注册进 GATT 表,栈起即可用;离开由 destroy_native_view 停。 */
    /* 同 show_voice:起 BLE 前先停 WiFi 腾堆,否则开机校时未完成时 WiFi 常驻
     * ~99KB,esp_hid(HID 服务)初始化会因堆不足失败 → 广播失败/扫不到。 */
    wifi_sta_stop();
    s_bt_enabled = true;
    nvs_set_bool("bt", true);
    /* PPT 页广播身份：名 "AI Passport"、广播 0xA2B0+0x1812(HID)、外观=
     * 键盘，主机按此识别为输入设备并持久配对（免反复配对）。 */
    ble_prov_set_identity(false);
    if (!ble_prov_is_running()) {
        ble_prov_start();
    }
    s_prev_view = s_view;
    s_ppt_pres_running = false;
    s_ppt_pres_seconds = 0;
    s_page = passport_ui_page_create("PPT 遥控", true, true);
    s_ppt_hint = passport_ui_label_create(s_page, "");
    s_ppt_action = passport_ui_label_create(s_page, "");
    s_ppt_timer_label = passport_ui_label_create(s_page, "00:00");
    ppt_timer_reset_display();

    lv_obj_t *content = passport_ui_page_content(s_page);
    (void)content;
    passport_ui_page_set_actions(s_page, "返回", "主页");
    passport_ui_page_show(s_page);
    ppt_refresh_status();
    ppt_set_action("按键控制演讲", 0xAAAAAA);
    if (!s_ppt_timer) s_ppt_timer = lv_timer_create(ppt_timer_cb, 500, NULL);
    s_view = VIEW_PPT;
}

/* 语音事件回调(passport_voice → 主队列;NimBLE/audio 任务上下文,零阻塞) */
static void on_voice_transcript(const char *text, bool final, void *user)
{
    (void)user;
    if (!s_events) return;
    system_event_t event = {.type = EVENT_VOICE_TRANSCRIPT};
    event.data.voice_transcript.final = final;
    snprintf(event.data.voice_transcript.text,
             sizeof(event.data.voice_transcript.text), "%s", text ? text : "");
    xQueueSend(s_events, &event, 0);
}

static void on_voice_link(bool up, void *user)
{
    (void)user;
    if (!s_events) return;
    system_event_t event = {.type = EVENT_VOICE_LINK};
    event.data.voice_link.up = up;
    xQueueSend(s_events, &event, 0);
}

static void on_voice_error(void *user)
{
    (void)user;
    if (!s_events) return;
    system_event_t event = {.type = EVENT_VOICE_ERROR};
    xQueueSend(s_events, &event, 0);
}

static void handle_key_event(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    /* 注:PPT 遥控页不做提前拦截,保留全局导航 —— 长按上键返回上一层、
     * 长按 OK 返回主页照常生效(离开页面即停 BLE,放映自然退出)。
     * 单击(上一页/下一页/开始放映)由下方 switch 的 case VIEW_PPT 交给
     * handle_ppt_key 处理。 */
    /* 长按确定：主页=息屏，子页面=返回主页 */
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        /* 配网页上长按 OK 即「取消」：终止后台配网任务 */
        if (s_view == VIEW_WIFI && s_wifi_view == WIFI_PROV) {
            s_wifi_cancel = true;
        }
        show_home();
        return;
    }
    /* 长按上键：主页=息屏，子页面=返回上一层 */
    if (btn == BSP_BTN_UP && ev == BSP_BTN_LONG) {
        if (s_view == VIEW_LAUNCHER) {
            human_display_pause();
            bsp_display_backlight(0);
            s_screen_off = true;
            return;
        }
        /* 子页面：返回上一层 */
        /* 语音会话中离开:录音当场收束(cancel,不发送) */
        if (s_view == VIEW_VOICE && s_voice_state == VOICE_RECORDING) {
            voice_finish_recording(false);
        }
        view_t back = s_prev_view;
        /* 如果是从网络与连接页进来的，返回网络与连接页 */
        if (s_was_on_network) {
            s_was_on_network = false;
            show_network();
            s_prev_view = VIEW_SETTINGS;
            return;
        }
        switch (back) {
        case VIEW_SETTINGS: show_settings(); return;
        case VIEW_APPS: show_apps(); return;
        default: show_home(); return;
        }
    }
    switch (s_view) {
    case VIEW_LAUNCHER: handle_launcher_key(btn, ev); break;
    case VIEW_APPS: handle_apps_key(btn, ev); break;
    case VIEW_SETTINGS: handle_settings_key(btn, ev); break;
case VIEW_DEVICE_INFO:
        if (s_on_network_page) {
            int delta = passport_input_navigation_delta(btn, ev);
            if (delta != 0 && s_list) {
                passport_ui_list_move(s_list, delta);
                break;
            }
            if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK && s_list) {
                size_t sel = passport_ui_list_selected(s_list);
                if (sel == 0) {
                    /* 蓝牙开关 = 两处开关之一(另一处在语音页,进则开/退则关)。
                     * 这里手动切换:开=立即启动 BLE 常驻,关=立即停栈。 */
                    s_bt_enabled = !s_bt_enabled;
                    nvs_set_bool("bt", s_bt_enabled);
                    if (s_bt_enabled) {
                        if (!ble_prov_is_running()) ble_prov_start();
                    } else {
                        if (s_wifi_task_alive) s_wifi_cancel = true;
                        if (ble_prov_is_running()) ble_stack_stop();
                    }
                    show_network();
                } else if (sel == 1) {
                    s_wifi_enabled = !s_wifi_enabled;
                    nvs_set_bool("wifi", s_wifi_enabled);
                    if (s_wifi_enabled) {
                        wifi_sta_init();
                        wifi_sta_set_auto_connect(s_wifi_enabled);
                    } else {
                        wifi_sta_stop();
                    }
                    show_network();
                } else if (sel == 2) {
                    s_was_on_network = true;
                    show_wifi();
                    return;
                } else if (sel == 3) {
                    s_was_on_network = true;
                    show_transfer_page();
                    return;
                }
            }
            break;
        }
        handle_device_info_key(btn, ev);
        break;
    case VIEW_THEMES: handle_themes_key(btn, ev); break;
    case VIEW_WIFI:
        if (s_wifi_view == WIFI_PROV) {
            /* 配网页:OK 单击在失败/内存不足(phase==4)时重试,否则忽略 */
            if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK && s_wifi_phase == 4) {
                show_wifi_prov();
            }
            break;
        }
        if (s_wifi_view == WIFI_INFO) {
            /* 长按下键:删除当前选中的已保存 WiFi 条目 */
            if (btn == BSP_BTN_DOWN && ev == BSP_BTN_LONG && s_list) {
                size_t sel = passport_ui_list_selected(s_list);
                char saved_ssids[WIFI_MAX_SAVED][33];
                int saved_count = wifi_sta_list_saved(saved_ssids, WIFI_MAX_SAVED);
                const char *target = NULL;
                if (wifi_sta_is_connected()) {
                    /* 列表: [当前, 切换到s1, 切换到s2, ..., 断开连接]
                     * 可删除 = 中间"切换到 X"项(排除当前连接项) */
                    int idx = 0;
                    for (int i = 0; i < saved_count; i++) {
                        if (strcmp(saved_ssids[i], wifi_sta_current_ssid()) == 0) continue;
                        if (idx == (int)sel - 1) { target = saved_ssids[i]; break; }
                        idx++;
                    }
                } else {
                    /* 列表: [s0, s1, ..., s(n-1), 热点配网];可删除 = 前 n 项 */
                    if (sel < (size_t)saved_count) target = saved_ssids[sel];
                }
                if (target && wifi_sta_delete_saved_by_ssid(target)) {
                    show_wifi();
                }
                break;
            }
            int delta = passport_input_navigation_delta(btn, ev);
            if (delta != 0 && s_list) {
                passport_ui_list_move(s_list, delta);
                break;
            }
            if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
                size_t sel = s_list ? passport_ui_list_selected(s_list) : 0;
                char saved_ssids[WIFI_MAX_SAVED][33];
                char saved_pass[65] = {0};
                int saved_count = wifi_sta_list_saved(saved_ssids, WIFI_MAX_SAVED);

if (wifi_sta_is_connected()) {
                    if (sel == 0) break;
                    if (sel == 1 + saved_count - 1) { wifi_sta_forget_creds(); wifi_sta_do_disconnect(); show_wifi(); return; }
                    int idx = 0;
                    char prev_ssid[33];
                    strlcpy(prev_ssid, wifi_sta_current_ssid(), sizeof(prev_ssid));
                    for (int i = 0; i < saved_count; i++) {
                        if (strcmp(saved_ssids[i], wifi_sta_current_ssid()) == 0) continue;
                        if (idx == (int)sel - 1) {
                            wifi_sta_get_saved_creds(saved_ssids[i], saved_pass, sizeof(saved_pass));
                            char **pair = malloc(3 * sizeof(char *));
                            pair[0] = strdup(saved_ssids[i]);
                            pair[1] = saved_pass[0] ? strdup(saved_pass) : NULL;
                            pair[2] = strdup(prev_ssid);
                            if (s_wifi_status) passport_ui_label_set_text(s_wifi_status, "正在连接...");
                            if (s_wifi_hint) passport_ui_label_set_text(s_wifi_hint, "长按 OK 取消");
                            if (xTaskCreate(wifi_connect_task, "wifi_conn", 4096, pair, 3, NULL) != pdPASS) {
                                free(pair[0]); free(pair[1]); free(pair[2]); free(pair);
                                if (s_wifi_status) passport_ui_label_set_text(s_wifi_status, "内存不足");
                            }
                            return;
                        }
                        idx++;
                    }
                } else {
                    if (sel < (size_t)saved_count) {
                        wifi_sta_get_saved_creds(saved_ssids[sel], saved_pass, sizeof(saved_pass));
                        char **pair = malloc(3 * sizeof(char *));
                        pair[0] = strdup(saved_ssids[sel]);
                        pair[1] = saved_pass[0] ? strdup(saved_pass) : NULL;
                        pair[2] = NULL;
                        if (s_wifi_status) passport_ui_label_set_text(s_wifi_status, "正在连接...");
                        if (s_wifi_hint) passport_ui_label_set_text(s_wifi_hint, "长按 OK 取消");
                        if (xTaskCreate(wifi_connect_task, "wifi_conn", 4096, pair, 3, NULL) != pdPASS) {
                            free(pair[0]); free(pair[1]); free(pair[2]); free(pair);
                            if (s_wifi_status) passport_ui_label_set_text(s_wifi_status, "内存不足");
                        }
                    } else {
                        show_wifi_prov();
                    }
                }
            } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG && !wifi_sta_is_connected()) {
                show_wifi_prov();
            }
        }
        break;
    case VIEW_TRANSFER: handle_transfer_key(btn, ev); break;
    case VIEW_VOICE: handle_voice_key(btn, ev); break;
    case VIEW_PPT: handle_ppt_key(btn, ev); break;
    default: break;
    }
}

static void on_button(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_events) return;
    system_event_t event = {.type = EVENT_KEY};
    event.data.key.btn = btn;
    event.data.key.ev = ev;
    xQueueSend(s_events, &event, 0);
}

static void system_task(void *arg)
{
    (void)arg;
    system_event_t event;
    while (xQueueReceive(s_events, &event, portMAX_DELAY) == pdTRUE) {
        if (event.type == EVENT_KEY &&
            consume_screen_wake(event.data.key.btn, event.data.key.ev)) {
            continue;
        }
        if (!bsp_lvgl_lock(1000)) continue;
        if (event.type == EVENT_KEY) {
            handle_key_event(event.data.key.btn, event.data.key.ev);
        } else if (event.type == EVENT_WIFI_CONNECT_DONE && s_view == VIEW_WIFI) {
            if (s_wifi_status) {
                if (event.data.wifi_connect.ok || wifi_sta_is_connected()) {
                    passport_ui_label_set_text(s_wifi_status, "连接成功");
                } else {
                    passport_ui_label_set_text(s_wifi_status, "连接失败，请重试");
                }
            }
            if (s_wifi_hint) passport_ui_label_set_text(s_wifi_hint, "长按 OK 返回");
        } else if (event.type == EVENT_VOICE_TRANSCRIPT) {
            /* 只在转写态落屏:退出转写/离开视图后的迟到文本一律丢弃 */
            if (s_view == VIEW_VOICE && s_voice_state == VOICE_TRANSCRIBING && s_voice_text) {
                lv_label_set_text(s_voice_text, event.data.voice_transcript.text);
            }
        } else if (event.type == EVENT_VOICE_LINK) {
            if (s_view == VIEW_VOICE) {
                if (!event.data.voice_link.up && s_voice_state == VOICE_RECORDING) {
                    voice_finish_recording(false);
                } else if (s_voice_state == VOICE_READY) {
                    voice_refresh_hint();
                }
            }
        } else if (event.type == EVENT_VOICE_ERROR) {
            if (s_view == VIEW_VOICE && s_voice_state == VOICE_RECORDING) {
                /* 采集硬件错误:管线已自停,收束回就绪 */
                voice_stop_timer();
                s_voice_state = VOICE_READY;
                if (s_voice_timer_label) lv_label_set_text(s_voice_timer_label, "");
                voice_set_hint("录音出错\n\n按住 ↓ 重试");
            }
        }
        bsp_lvgl_unlock();
        if (event.type == EVENT_KEY &&
            is_key_sound_event(event.data.key.btn, event.data.key.ev)) {
            passport_settings_key_feedback();
        }
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static void screen_state_cb(bool off)
{
    if (bsp_lvgl_lock(100)) {
        if (off) human_display_pause(); else human_display_resume();
        bsp_lvgl_unlock();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Passport Platform v1 启动");
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(passport_identity_init());
    ESP_ERROR_CHECK(bsp_i2c_init());

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示初始化失败，系统无法启动");
        return;
    }
    esp_err_t settings_err = passport_settings_init();
    if (settings_err != ESP_OK) {
        ESP_LOGE(TAG, "设置服务初始化不完整: %s", esp_err_to_name(settings_err));
    }

    esp_err_t storage_err = passport_storage_init();
    if (storage_err != ESP_OK) ESP_LOGE(TAG, "存储分区不可用: %s", esp_err_to_name(storage_err));
    bool battery_ok = bsp_battery_init() == ESP_OK;
    passport_theme_init();
    passport_ui_init(battery_ok);

    s_events = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(system_event_t));
    if (!s_events) {
        ESP_LOGE(TAG, "系统事件队列创建失败");
        return;
    }
    if (xTaskCreate(system_task, "passport_system", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "系统任务创建失败");
        return;
    }

    s_bt_enabled = nvs_get_bool("bt", true);
    s_wifi_enabled = nvs_get_bool("wifi", true);

    /* 语音功能:codec 守卫依赖设置服务;事件回调只入主队列(零阻塞) */
    passport_voice_set_transcript_callback(on_voice_transcript, NULL);
    passport_voice_set_link_callback(on_voice_link, NULL);
    passport_voice_set_error_callback(on_voice_error, NULL);
    if (passport_voice_init() != ESP_OK) {
        ESP_LOGW(TAG, "语音功能不可用(音频初始化失败)");
    }

    ESP_ERROR_CHECK(bsp_button_init(on_button, NULL));
    /* BLE 不常驻:进语音输入页由 show_voice 启用(进则开),离开即停(退则关);
     * 设置页也可手动启停。s_bt_enabled 记录当前开关状态(两处开关之一)。
     * WiFi 同样按需启用/退出即关:启动仅用于校时,完成后自动释放;需要联网的页面
     * (传输/配网)进入时临时启用,离开即停。这样常驻内存占用最小,给 BLE 配对腾堆。 */
    wifi_sta_set_auto_release(s_wifi_enabled);
    if (s_wifi_enabled && wifi_sta_init() != ESP_OK) {
        ESP_LOGW(TAG, "WiFi 初始化失败，WiFi 功能不可用");
    }

    passport_settings_set_screen_callback(screen_state_cb);

    if (bsp_lvgl_lock(1000)) {
        show_home();
        bsp_lvgl_unlock();
    }
    ESP_LOGI(TAG, "系统就绪，设备码=%s", passport_identity_code());
}
