// components/passport_ppt/src/passport_ppt.c
// BLE HID 键盘服务（esp_hid 组件 · NimBLE 后端）。
// 移植自 ai-passport-ppt main/ble_hid.c，但协议栈按 o-platform 平台归位：
//   donor 用 Bluedroid + esp_hid；本平台 NimBLE 生命周期归 ble_prov_start() 独占，
//   故这里用 esp_hid 组件的 NimBLE 后端（esp_hidd_dev_init），它内部经
//   ble_svc_hid_add 注册标准 HID 服务(0x1812) + BAS + DIS，发送标准 8 字节
//   Boot Keyboard 报告，供 Windows / macOS / WPS / LibreOffice 识别为键盘。
//
// ⚠️ 为什么不用手写 NimBLE GATT：ai-passport-ppt 的开发日志(见其
//    docs/development-log.md)实锤——手写 NimBLE GATT HID 在 Windows 上
//    只能连上、能发 notify，但 Windows 一律认成"其他设备"、不接收按键；
//    只有官方 esp_hid 组件注册的标准 HID GATT 才能被识别为键盘。
//    本组件 IDF5.5.3 的 esp_hid 自带 nimble_hidd.c 后端（CONFIG_BT_NIMBLE_HID_SERVICE）。
//
// 移植出处：https://github.com/YeatsLiao/ai-passport-ppt （MIT）

#include "passport_ppt.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_hidd.h"
#include "esp_hid_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "passport_ppt";

#define DEVICE_NAME "PPT-Remote"
#define HID_KEYBOARD_REPORT_ID 1

// 击键时序（macOS 对组合键按住时长要求较宽，加大保证双平台可靠）
#define KEY_HOLD_MS       120
#define KEY_COMBO_GAP_MS  200

// ---- 状态 ----
static esp_hidd_dev_t *s_hid_dev = NULL;
static volatile bool s_connected = false;
static volatile bool s_auth_ok = false;
static volatile bool s_bonded = false;

// ---- Report Map（标准 Boot Keyboard 报告，Report ID = 1） ----
// 与 donor ble_hid.c 的 s_keyboard_report_map 原值一致。
static const unsigned char s_keyboard_report_map[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Kbrd/Keypad)
    0x19, 0xE0,        //   Usage Minimum (0xE0)
    0x29, 0xE7,        //   Usage Maximum (0xE7)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)          ← modifier 字节
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Const)                 ← reserved 字节
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x05,        //   Usage Maximum (Kana)
    0x91, 0x02,        //   Output (Data,Var,Abs)         ← LED 报告
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x01,        //   Output (Const)                ← LED 填充位
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Kbrd/Keypad)
    0x19, 0x00,        //   Usage Minimum (0x00)
    0x29, 0x65,        //   Usage Maximum (0x65)
    0x81, 0x00,        //   Input (Data,Array)            ← 6 个按键槽
    0xC0,              // End Collection
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    { .data = s_keyboard_report_map, .len = sizeof(s_keyboard_report_map) },
};

static esp_hid_device_config_t s_hid_config = {
    .vendor_id          = 0x16C0,
    .product_id         = 0x05DF,
    .version            = 0x0100,
    .device_name        = DEVICE_NAME,
    .manufacturer_name  = "FoloToy",
    .serial_number      = "1234567890",
    .report_maps        = s_report_maps,
    .report_maps_len    = 1,
};

// ---- GAP 事件（认证状态追踪） ----
// esp_hid 的 NimBLE 后端自己注册了 CONNECT/DISCONNECT 监听；这里再挂一个
// 监听，专门追踪 ENC_CHANGE（配对加密完成 = 认证通过，Windows 要求加密后才
// 接受 HID 报告）。多个 ble_gap_event_listener 可并存。
static struct ble_gap_event_listener s_gap_listener;
static int ppt_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            s_auth_ok = true;
            s_bonded = true;
            ESP_LOGI(TAG, "HID 加密/认证成功 (conn %u)",
                     event->enc_change.conn_handle);
        } else {
            s_auth_ok = false;
            ESP_LOGE(TAG, "HID 加密失败 rc=%d", event->enc_change.status);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        s_auth_ok = false;
        ESP_LOGI(TAG, "HID 主机断开 (reason %d)", event->disconnect.reason);
        return 0;
    default:
        return 0;
    }
}

// ---- esp_hid 设备事件回调 ----
static void hidd_event_callback(void *handler_args, esp_event_base_t base,
                                int32_t id, void *event_data)
{
    (void)handler_args; (void)base;
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    (void)event_data;
    switch (event) {
    case ESP_HIDD_CONNECT_EVENT:
        s_connected = true;
        ESP_LOGI(TAG, "HID 主机已连接");
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        s_connected = false;
        s_auth_ok = false;
        ESP_LOGI(TAG, "HID 主机断开");
        break;
    case ESP_HIDD_PROTOCOL_MODE_EVENT:
        ESP_LOGI(TAG, "HID 协议模式=%s", "REPORT");
        break;
    default:
        break;
    }
}

// ---- 对外接口 ----
esp_err_t passport_ppt_register(void)
{
    if (s_hid_dev) return ESP_OK;   // 已注册

    /* 调用 esp_hidd_dev_init：内部经 nimble_hid_start_gatts 注册标准
     * HID(0x1812)+BAS(0x180F)+DIS(0x180A)+GAP+GATT 服务，并把
     * ble_hs_cfg.gatts_register_cb 设为它的实现（用于捕获 report handle，
     * input_set 依赖此 handle）。sync_cb/reset_cb 随后由 ble_prov 覆盖，
     * 不影响功能（广播由 ble_prov 管理）。 */
    esp_err_t ret = esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE,
                                      hidd_event_callback, &s_hid_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidd_dev_init 失败: %s", esp_err_to_name(ret));
        s_hid_dev = NULL;
        return ret;
    }

    // 追加认证追踪监听
    int rc = ble_gap_event_listener_register(&s_gap_listener, ppt_gap_event, NULL);
    if (rc != 0) ESP_LOGW(TAG, "GAP 监听注册失败 %d", rc);

    ESP_LOGI(TAG, "HID 键盘服务就绪 (esp_hid/NimBLE, 0x1812)");
    return ESP_OK;
}

esp_err_t passport_ppt_deinit(void)
{
    if (!s_hid_dev) return ESP_OK;
    esp_err_t ret = esp_hidd_dev_deinit(s_hid_dev);
    s_hid_dev = NULL;
    s_connected = false;
    s_auth_ok = false;
    s_bonded = false;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidd_dev_deinit 失败: %s", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "HID 键盘服务已卸载");
    return ret;
}

// 内部：发送一次带修饰键的击键（按下 → 延时 → 释放）
static void send_key_press(uint8_t modifier, uint8_t keycode)
{
    if (s_hid_dev == NULL || !s_connected || !s_auth_ok) {
        ESP_LOGW(TAG, "Key 0x%02X ignored: not connected/authenticated", keycode);
        return;
    }

    // 8 字节报告: [modifier][reserved][key0..key5]
    uint8_t report[8] = {0};
    report[0] = modifier;
    report[2] = keycode;  // 按下

    esp_err_t err = esp_hidd_dev_input_set(s_hid_dev, 0, HID_KEYBOARD_REPORT_ID,
                                           report, sizeof(report));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "input_set (press) 失败: %d", err);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(KEY_HOLD_MS));

    memset(report, 0, sizeof(report));  // 释放
    err = esp_hidd_dev_input_set(s_hid_dev, 0, HID_KEYBOARD_REPORT_ID,
                                 report, sizeof(report));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "input_set (release) 失败: %d", err);
    }
}

void passport_ppt_key_press(uint8_t modifier, uint8_t keycode)
{
    send_key_press(modifier, keycode);
}

void passport_ppt_press_start_slideshow(void)
{
    // 依次发送三套快捷键，覆盖主流演示软件：
    //   1. F5                    → Windows PowerPoint / WPS / LibreOffice
    //   2. Cmd+Shift+Return      → macOS PowerPoint
    //   3. Cmd+Alt+P             → macOS Keynote
    send_key_press(0, PPT_HID_KEY_F5);
    vTaskDelay(pdMS_TO_TICKS(KEY_COMBO_GAP_MS));
    send_key_press(PPT_HID_MOD_L_GUI | PPT_HID_MOD_L_SHIFT, PPT_HID_KEY_RETURN);
    vTaskDelay(pdMS_TO_TICKS(KEY_COMBO_GAP_MS));
    send_key_press(PPT_HID_MOD_L_GUI | PPT_HID_MOD_L_ALT, PPT_HID_KEY_P);
}

bool passport_ppt_is_connected(void)
{
    return s_connected && s_auth_ok;
}

bool passport_ppt_is_paired(void)
{
    return s_bonded;
}
