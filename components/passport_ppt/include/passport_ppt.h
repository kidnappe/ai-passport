// components/passport_ppt/include/passport_ppt.h
// BLE HID 键盘服务（esp_hid 组件 · NimBLE 后端）。
// 由 esp_hidd_dev_init 注册官方标准 HID GATT（0x1812 + BAS + DIS），
// 发送键盘报告走 esp_hidd_dev_input_set（Report 特征通知）。
// 连接/加密状态经 GAP 事件监听回传。
//
// 移植出处：https://github.com/YeatsLiao/ai-passport-ppt （MIT）
//   donor 文件 main/ble_hid.c → 本组件 src/passport_ppt.c
//   协议栈按平台归位：Bluedroid esp_hid → NimBLE esp_hid 后端。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// HID 键码（Usage Tables）
#define PPT_HID_KEY_LEFT_ARROW  0x50
#define PPT_HID_KEY_RIGHT_ARROW 0x4F
#define PPT_HID_KEY_ESCAPE      0x29
#define PPT_HID_KEY_F5          0x3E
#define PPT_HID_KEY_RETURN      0x28
#define PPT_HID_KEY_P           0x13

// 修饰键位图
#define PPT_HID_MOD_L_SHIFT     0x02
#define PPT_HID_MOD_L_ALT       0x04
#define PPT_HID_MOD_L_GUI       0x08

/**
 * @brief 注册 HID 键盘服务（在 nimble_port_init() 之后、
 *        nimble_port_freertos_init() 之前调用，与 voice_ble_register 同契约）。
 *        需先启用 CONFIG_BT_NIMBLE_HID_SERVICE。
 * @return ESP_OK 成功
 */
esp_err_t passport_ppt_register(void);

/**
 * @brief 卸载 HID 键盘服务（在 ble_stack_stop 反初始化 NimBLE 前调用，
 *        使下次 ble_prov_start 能重新注册）。未注册时为空操作。
 * @return ESP_OK 成功
 */
esp_err_t passport_ppt_deinit(void);

/**
 * @brief 发送一次击键（按下 → 延时 → 释放）。
 * @param modifier HID 修饰键位图（0 = 无修饰）
 * @param keycode  HID Usage ID（如 PPT_HID_KEY_LEFT_ARROW）
 * @note 未连接/未认证时忽略（打日志），不阻塞调用方太久（击键时序见实现）。
 */
void passport_ppt_key_press(uint8_t modifier, uint8_t keycode);

/**
 * @brief 发送"开始放映"跨平台组合键（F5；Cmd+Shift+Return；Cmd+Alt+P）。
 */
void passport_ppt_press_start_slideshow(void);

/**
 * @brief 是否已有 HID 主机连接（连接 + 认证完成）。
 */
bool passport_ppt_is_connected(void);

/**
 * @brief 是否已配好对（SM 绑定完成）。
 */
bool passport_ppt_is_paired(void);

#ifdef __cplusplus
}
#endif
