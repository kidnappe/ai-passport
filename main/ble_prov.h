// main/ble_prov.h —— 蓝牙(BLE/NimBLE)配网 + 语音通道(0xA2B0)共用的常驻 BLE 栈
// 手机用 BLE 调试工具(nRF Connect 等)连接本设备，向配网特征写入两行文本：
//   第1行: WiFi 名称(SSID)
//   第2行: 密码(开放网络留空)
// 语音功能(passport_voice)与配网共用同一 NimBLE 实例：栈由 ble_prov_start()
// 常驻启动，语音 GATT 服务(0xA2B0)同时注册，广播名 "AI Passport" 同时服务
// 语音桌面端发现与配网发现(配网按扫描应答中的 128-bit 服务 UUID 过滤)。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

// 启动 BLE 栈（配网 + 语音双服务注册）与广播。幂等：栈已在跑则只重启广播。
esp_err_t ble_prov_start(void);

// 配网结束：保留 BLE 栈（语音通道需要常驻），断开配网连接并重启广播。
esp_err_t ble_prov_stop(void);

// 完整停栈（设置里关闭蓝牙开关时调用）：反初始化 NimBLE，语音通道随之关闭。
esp_err_t ble_stack_stop(void);

// 是否正在运行
bool ble_prov_is_running(void);

// 广播身份：语音页 vs PPT 页在广播层分离（名字/服务 UUID/外观不同），
// 避免 Windows 把语音广播误判为 HID 键盘（已配对设备走特殊重连通道，
// 通用 BLE 扫描 API 扫不到）。
//   - true(语音页): 名 "AI Passport Voice"，只广播 0xA2B0，外观=普通外设；
//   - false(PPT 页): 名 "AI Passport"，广播 0xA2B0+0x1812，外观=键盘。
// 设值后立即按新身份重启广播（栈已跑时）；栈未跑则仅记录，start 时生效。
void ble_prov_set_identity(bool is_voice);

// 取走收到的凭证（读取后内部标志清除，只返回一次 true；未收到返回 false）
bool ble_prov_get_creds(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz);
