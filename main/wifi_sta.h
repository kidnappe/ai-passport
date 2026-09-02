// main/wifi_sta.h —— WiFi STA 模块：扫描 + 连接 + NVS 存储
#pragma once

#include "esp_err.h"
#include "esp_wifi.h"
#include <stdbool.h>

#define WIFI_SCAN_MAX  20

typedef struct {
    char             ssid[33];
    int8_t           rssi;
    wifi_auth_mode_t authmode;
    uint8_t          primary;
} wifi_ap_info_t;

typedef enum {
    WIFI_STA_EVT_CONNECTED,
    WIFI_STA_EVT_DISCONNECTED,
    WIFI_STA_EVT_SCAN_DONE,
    WIFI_STA_EVT_FAIL,
} wifi_sta_evt_t;

typedef void (*wifi_sta_cb_t)(wifi_sta_evt_t evt, void *data, void *user);

int wifi_sta_scan(wifi_ap_info_t *out, int max_count);
esp_err_t wifi_sta_connect(const char *ssid, const char *password);
esp_err_t wifi_sta_do_disconnect(void);
bool wifi_sta_is_connected(void);
const char *wifi_sta_current_ssid(void);
void wifi_sta_register_cb(wifi_sta_cb_t cb, void *user);
void wifi_sta_unregister_cb(wifi_sta_cb_t cb);
void wifi_sta_stop(void);
void wifi_sta_set_suspended(bool en);
bool wifi_sta_is_suspended(void);
esp_err_t wifi_sta_init(void);
void wifi_sta_set_auto_connect(bool on);
esp_err_t wifi_sta_connect_default(void);
/* 校时(SMTP)完成后自动释放 WiFi(esp_wifi_deinit),用于"按需启用/退出即关"模式 */
void wifi_sta_set_auto_release(bool en);
bool wifi_sta_get_saved_creds(const char *ssid, char *pass, size_t pass_sz);
#define WIFI_MAX_SAVED 8
int wifi_sta_list_saved(char ssids[][33], int max_count);
bool wifi_sta_delete_saved_by_ssid(const char *ssid);
void wifi_sta_save_creds(const char *ssid, const char *pass);
void wifi_sta_forget_creds(void);
void wifi_sta_clear_no_auto(void);
