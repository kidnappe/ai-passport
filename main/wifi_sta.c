// main/wifi_sta.c —— WiFi STA 模块：扫描 + 连接 + NVS 存储
#include "wifi_sta.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_task_wdt.h"
#include <string.h>
#include <time.h>

static inline size_t strlcpy_local(char *dst, const char *src, size_t dstsize) {
    if (dstsize == 0) return strlen(src);
    size_t n = strlen(src);
    if (n >= dstsize) n = dstsize - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return strlen(src);
}
#define strlcpy(dst, src, dstsize) strlcpy_local(dst, src, dstsize)

static const char *TAG = "wifi_sta";
static const char *NVS_NS = "wifi_sta";

static SemaphoreHandle_t s_nvs_mutex;
static SemaphoreHandle_t s_got_ip;
static SemaphoreHandle_t s_conn_mutex;   // 串行化连接流程, 防多任务并发 connect
static volatile bool s_connected = false;
static bool s_inited = false;
static bool s_once = false;   // netif/事件处理器只需创建一次, 反复 init/deinit 会泄漏
static char s_cur_ssid[33] = {0};
static volatile bool s_suspended = false;   // 配网期间挂起自动重连
static bool s_auto_connect = true;
static volatile bool s_release_after_sync = false; // 校时完成后自动释放 WiFi(按需启用模式)
static int s_retry_count = 0;
#define WIFI_MAX_RETRY 3

static wifi_sta_cb_t s_user_cb = NULL;
static void *s_user_data = NULL;

static void fire_evt(wifi_sta_evt_t evt, void *data) {
    if (s_user_cb) s_user_cb(evt, data, s_user_data);
}

static esp_err_t nvs_lock(TickType_t wait) {
    if (!s_nvs_mutex) s_nvs_mutex = xSemaphoreCreateMutex();
    return xSemaphoreTake(s_nvs_mutex, wait) ? ESP_OK : ESP_ERR_TIMEOUT;
}
static void nvs_unlock(void) {
    if (s_nvs_mutex) xSemaphoreGive(s_nvs_mutex);
}

void wifi_sta_forget_creds(void) {
    if (nvs_lock(portMAX_DELAY) != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "no_auto", 1);
        nvs_commit(h);
        nvs_close(h);
    }
    nvs_unlock();
    s_auto_connect = false;
    ESP_LOGI(TAG, "已标记不自动重连");
}

void wifi_sta_save_creds(const char *ssid, const char *pass) {
    if (nvs_lock(portMAX_DELAY) != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        char existing[33];
        int32_t count = 0;
        size_t sz;
        nvs_get_i32(h, "count", (int32_t *)&count);

        // 如果已存在，删除旧条目
        for (int i = 0; i < count; i++) {
            char key[20];
            snprintf(key, sizeof(key), "ssid_%d", i);
            sz = sizeof(existing);
            if (nvs_get_str(h, key, existing, &sz) == ESP_OK && strcmp(existing, ssid) == 0) {
                nvs_erase_key(h, key);
                snprintf(key, sizeof(key), "pass_%d", i);
                nvs_erase_key(h, key);
                // 压缩后续条目
                for (int j = i + 1; j < count; j++) {
                    char old_key[20], new_key[20];
                    snprintf(old_key, sizeof(old_key), "ssid_%d", j);
                    snprintf(new_key, sizeof(new_key), "ssid_%d", j - 1);
                    sz = sizeof(existing);
                    if (nvs_get_str(h, old_key, existing, &sz) == ESP_OK) {
                        nvs_set_str(h, new_key, existing);
                    }
                    snprintf(old_key, sizeof(old_key), "pass_%d", j);
                    snprintf(new_key, sizeof(new_key), "pass_%d", j - 1);
                    if (nvs_get_str(h, old_key, existing, &sz) == ESP_OK) {
                        nvs_set_str(h, new_key, existing);
                    }
                }
                count--;
                break;
            }
        }

        // 添加到末尾
        if (count < WIFI_MAX_SAVED) {
            char key[20];
            snprintf(key, sizeof(key), "ssid_%d", (int)count);
            nvs_set_str(h, key, ssid);
            snprintf(key, sizeof(key), "pass_%d", (int)count);
            if (pass && pass[0]) nvs_set_str(h, key, pass);
            else nvs_erase_key(h, key);
            count++;
            nvs_set_i32(h, "count", count);
        }
        nvs_commit(h);
        nvs_close(h);
    }
    nvs_unlock();
}

/* 按 SSID 删除单个已保存条目(长按下键删除用)。删除后压缩后续条目。
 * 若删除的是当前已连接的 SSID,调用方需自行决定是否断开。 */
bool wifi_sta_delete_saved_by_ssid(const char *ssid) {
    if (!ssid || !ssid[0]) return false;
    if (nvs_lock(portMAX_DELAY) != ESP_OK) return false;
    nvs_handle_t h;
    bool removed = false;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        int32_t count = 0;
        size_t sz;
        nvs_get_i32(h, "count", (int32_t *)&count);
        for (int i = 0; i < count; i++) {
            char key[20], existing[33];
            snprintf(key, sizeof(key), "ssid_%d", i);
            sz = sizeof(existing);
            if (nvs_get_str(h, key, existing, &sz) == ESP_OK && strcmp(existing, ssid) == 0) {
                nvs_erase_key(h, key);
                snprintf(key, sizeof(key), "pass_%d", i);
                nvs_erase_key(h, key);
                /* 压缩后续条目 */
                for (int j = i + 1; j < count; j++) {
                    char old_key[20], new_key[20];
                    snprintf(old_key, sizeof(old_key), "ssid_%d", j);
                    snprintf(new_key, sizeof(new_key), "ssid_%d", j - 1);
                    sz = sizeof(existing);
                    if (nvs_get_str(h, old_key, existing, &sz) == ESP_OK) {
                        nvs_set_str(h, new_key, existing);
                        nvs_erase_key(h, old_key);
                    }
                    snprintf(old_key, sizeof(old_key), "pass_%d", j);
                    snprintf(new_key, sizeof(new_key), "pass_%d", j - 1);
                    sz = sizeof(existing);
                    if (nvs_get_str(h, old_key, existing, &sz) == ESP_OK) {
                        nvs_set_str(h, new_key, existing);
                        nvs_erase_key(h, old_key);
                    }
                }
                count--;
                nvs_set_i32(h, "count", count);
                removed = true;
                break;
            }
        }
        nvs_commit(h);
        nvs_close(h);
    }
    nvs_unlock();
    if (removed) ESP_LOGI(TAG, "已删除保存的 WiFi: %s", ssid);
    return removed;
}

int wifi_sta_list_saved(char ssids[][33], int max_count) {
    if (nvs_lock(portMAX_DELAY) != ESP_OK) return 0;
    nvs_handle_t h;
    int32_t count = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "count", (int32_t *)&count);
        if (count > max_count) count = max_count;
        for (int i = 0; i < count; i++) {
            char key[20];
            snprintf(key, sizeof(key), "ssid_%d", i);
            size_t sz = 33;
            nvs_get_str(h, key, ssids[i], &sz);
        }
        nvs_close(h);
    }
    nvs_unlock();
    return count;
}

static void save_creds(const char *ssid, const char *pass) {
    wifi_sta_save_creds(ssid, pass);
}

static bool load_creds(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz) {
    if (nvs_lock(portMAX_DELAY) != ESP_OK) return false;
    nvs_handle_t h;
    bool ok = false;
    // Load the most recent (last entry)
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t count = 0;
        size_t sz = sizeof(count);
        if (nvs_get_i32(h, "count", (int32_t *)&count) == ESP_OK && count > 0) {
            char key[20];
            snprintf(key, sizeof(key), "ssid_%d", (int)(count - 1));
            sz = ssid_sz;
            if (nvs_get_str(h, key, ssid, &sz) == ESP_OK) {
                snprintf(key, sizeof(key), "pass_%d", (int)(count - 1));
                sz = pass_sz;
                esp_err_t err = nvs_get_str(h, key, pass, &sz);
                if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) ok = true;
            }
        }
        nvs_close(h);
    }
    nvs_unlock();
    return ok;
}

/* 校时检测任务:轮询 SNTP 同步状态,完成后自动释放 WiFi 腾出内存。
 * 用于"启动校时后 WiFi 不常驻"的按需启用模式。 */
static void wifi_sync_release_task(void *arg);

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            if (!s_auto_connect) return;
            int32_t no_auto = 0;
            if (nvs_lock(portMAX_DELAY) == ESP_OK) {
                nvs_handle_t h;
                if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
                    nvs_get_i32(h, "no_auto", &no_auto);
                    nvs_close(h);
                }
                nvs_unlock();
            }
            if (no_auto) return;
            char ssid[33] = {0}, pass[65] = {0};
            if (load_creds(ssid, sizeof(ssid), pass, sizeof(pass)) && ssid[0]) {
                wifi_config_t wc = {0};
                strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
                if (pass[0]) {
                    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
                    wc.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
                } else {
                    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
                }
                esp_wifi_set_config(WIFI_IF_STA, &wc);
                strlcpy(s_cur_ssid, ssid, sizeof(s_cur_ssid));
                ESP_LOGI(TAG, "自动连接: %s", ssid);
                esp_wifi_connect();
            }
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_connected = false;
            wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)data;
            ESP_LOGI(TAG, "断线, reason=%d", disc ? disc->reason : -1);
            fire_evt(WIFI_STA_EVT_DISCONNECTED, data);
            if (!s_suspended && s_retry_count < WIFI_MAX_RETRY) {
                s_retry_count++;
                ESP_LOGI(TAG, "断线重连 (%d/%d)...", s_retry_count, WIFI_MAX_RETRY);
                esp_wifi_connect();
            }
        } else if (id == WIFI_EVENT_SCAN_DONE) {
            fire_evt(WIFI_STA_EVT_SCAN_DONE, NULL);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        s_retry_count = 0;
        xSemaphoreGive(s_got_ip);
        fire_evt(WIFI_STA_EVT_CONNECTED, data);
        /* Initialize SNTP for time sync */
        static bool sntp_started = false;
        if (!sntp_started) {
            sntp_started = true;
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "ntp.aliyun.com");
            esp_sntp_setservername(1, "ntp.tencent.com");
            esp_sntp_setservername(2, "pool.ntp.org");
            esp_sntp_init();
            ESP_LOGI(TAG, "SNTP 已启动，正在校时...");
            /* Set timezone to China */
            setenv("TZ", "CST-8", 1);
            tzset();
            if (s_release_after_sync) {
                /* 按需启用模式:校时完成后自动释放 WiFi(不常驻) */
                xTaskCreate(wifi_sync_release_task, "wifi_rel", 2048, NULL, 5, NULL);
            }
        }
    }
}

void wifi_sta_set_auto_connect(bool on) { s_auto_connect = on; }
void wifi_sta_set_auto_release(bool en) { s_release_after_sync = en; }

/* 校时检测任务:轮询 SNTP 同步状态,完成后自动释放 WiFi 腾出内存。
 * 用于"启动校时后 WiFi 不常驻"的按需启用模式。 */
static void wifi_sync_release_task(void *arg)
{
    esp_task_wdt_delete(NULL);
    int waited = 0;
    while (waited < 40) {          /* 最多等 20s */
        if (!s_inited) break;       /* WiFi 已被其它路径释放 */
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            ESP_LOGI(TAG, "校时完成, 自动释放 WiFi");
            wifi_sta_stop();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        waited++;
    }
    /* 超时也释放:SNTP 未完成(断网/无 NTP 服务器可达)时 WiFi 不能常驻占堆,
     * 否则 BLE 启动(NimBLE+esp_hid+语音 GATT)会因堆不足失败 → 广播起不来、
     * 桌面端扫不到。释放后进入语音/PPT 页时由 show_voice/show_ppt 保证干净堆。 */
    if (s_inited && sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "校时超时, 强制释放 WiFi 腾堆");
        wifi_sta_stop();
    }
    vTaskDelete(NULL);
}

void wifi_sta_clear_no_auto(void) {
    if (nvs_lock(portMAX_DELAY) != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "no_auto");
        nvs_commit(h);
        nvs_close(h);
    }
    nvs_unlock();
}

esp_err_t wifi_sta_init(void) {
#if CONFIG_O_PLATFORM_SIM_MODE
    /* QEMU 无射频: esp_wifi_start 后 wifi 任务会在校准处死循环并饿死 CPU */
    ESP_LOGI(TAG, "仿真构建: 跳过 WiFi 初始化(QEMU 无射频)");
    return ESP_OK;
#endif
    if (s_inited) return ESP_OK;

    if (!s_once) {
        esp_netif_init();
        esp_event_loop_create_default();
        esp_netif_create_default_wifi_sta();
        s_got_ip = xSemaphoreCreateBinary();
        if (!s_got_ip) return ESP_ERR_NO_MEM;
        s_conn_mutex = xSemaphoreCreateMutex();
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));
        s_once = true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t w_err = esp_wifi_init(&cfg);
    if (w_err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 初始化失败: %s", esp_err_to_name(w_err));
        return w_err;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_country_t country = { .cc="CN", .schan=1, .nchan=13, .policy=WIFI_COUNTRY_POLICY_AUTO };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    vTaskDelay(pdMS_TO_TICKS(50));

    s_inited = true;
    s_suspended = false; /* 重新上电后恢复断线自动重连 */
    ESP_LOGI(TAG, "WiFi STA 初始化完成");
    return ESP_OK;
}

int wifi_sta_scan(wifi_ap_info_t *out, int max_count) {
#if CONFIG_O_PLATFORM_SIM_MODE
    return 0;
#endif
    if (wifi_sta_init() != ESP_OK) return 0;

    wifi_scan_config_t cfg = { 0 };
    cfg.show_hidden = false;

    ESP_LOGI(TAG, "开始扫描...");
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "扫描启动失败: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t ap_cnt = 0;
    esp_wifi_scan_get_ap_num(&ap_cnt);
    if (ap_cnt == 0) { ESP_LOGW(TAG, "未发现 AP"); return 0; }

    uint16_t fetch = ap_cnt < WIFI_SCAN_MAX ? ap_cnt : WIFI_SCAN_MAX;
    wifi_ap_record_t *rec = calloc(fetch, sizeof(wifi_ap_record_t));
    if (!rec) return 0;
    esp_wifi_scan_get_ap_records(&fetch, rec);

    int cnt = 0;
    for (int i = 0; i < fetch && cnt < max_count; i++) {
        strlcpy(out[cnt].ssid, (const char *)rec[i].ssid, sizeof(out[cnt].ssid));
        out[cnt].rssi = rec[i].rssi;
        out[cnt].authmode = rec[i].authmode;
        out[cnt].primary = rec[i].primary;
        cnt++;
    }
    free(rec);
    ESP_LOGI(TAG, "扫描完成: %d 个 AP", cnt);
    return cnt;
}

void wifi_sta_set_suspended(bool en) { s_suspended = en; }
bool wifi_sta_is_suspended(void)     { return s_suspended; }

esp_err_t wifi_sta_connect(const char *ssid, const char *pass) {
#if CONFIG_O_PLATFORM_SIM_MODE
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (s_suspended) return ESP_ERR_INVALID_STATE;
    if (wifi_sta_init() != ESP_OK) return ESP_ERR_NO_MEM;

    // 串行化: keepalive 任务与 AI 请求可能同时触发连接
    if (s_conn_mutex && xSemaphoreTake(s_conn_mutex, pdMS_TO_TICKS(5000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    esp_err_t ret = ESP_OK;
    if (s_connected && strcmp(s_cur_ssid, ssid) == 0) {
        strlcpy(s_cur_ssid, ssid, sizeof(s_cur_ssid));
        wifi_sta_clear_no_auto();
        ret = ESP_OK;                       // 已连同一网络
        goto out;
    }

    if (s_connected) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    if (pass && pass[0]) {
        strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
        wc.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    } else {
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (cfg_err != ESP_OK) {
        ESP_LOGE(TAG, "设置 WiFi 配置失败: %s", esp_err_to_name(cfg_err));
        ret = cfg_err;
        goto out;
    }
    ESP_LOGI(TAG, "连接 %s ...", ssid);

    xSemaphoreTake(s_got_ip, 0);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "连接启动失败: %s", esp_err_to_name(err));
        ret = err;
        goto out;
    }

    if (xSemaphoreTake(s_got_ip, pdMS_TO_TICKS(30000)) == pdTRUE) {
        strlcpy(s_cur_ssid, ssid, sizeof(s_cur_ssid));
        s_retry_count = 0;
        save_creds(ssid, pass);
        wifi_sta_clear_no_auto();
        ESP_LOGI(TAG, "已连接 %s", ssid);
        ret = ESP_OK;
    } else {
        ESP_LOGE(TAG, "连接 %s 超时", ssid);
        ret = ESP_ERR_TIMEOUT;
    }
out:
    xSemaphoreGive(s_conn_mutex);
    return ret;
}

esp_err_t wifi_sta_autoconnect(void) {
#if CONFIG_O_PLATFORM_SIM_MODE
    return ESP_ERR_NOT_SUPPORTED;
#endif
    char ssid[33] = {0}, pass[65] = {0};
    if (load_creds(ssid, sizeof(ssid), pass, sizeof(pass)) && ssid[0]) {
        ESP_LOGI(TAG, "自动连接: %s", ssid);
        return wifi_sta_connect(ssid, pass[0] ? pass : NULL);
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t wifi_sta_do_disconnect(void) {
#if CONFIG_O_PLATFORM_SIM_MODE
    return ESP_OK;
#endif
    if (s_connected) { s_connected = false; return esp_wifi_disconnect(); }
    return ESP_OK;
}

bool wifi_sta_is_connected(void) { return s_connected; }
const char *wifi_sta_current_ssid(void) { return s_cur_ssid; }

bool wifi_sta_get_saved_creds(const char *ssid, char *pass, size_t pass_sz) {
    if (nvs_lock(portMAX_DELAY) != ESP_OK) return false;
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t count = 0;
        size_t sz = sizeof(count);
        if (nvs_get_i32(h, "count", (int32_t *)&count) == ESP_OK) {
            for (int i = 0; i < count; i++) {
                char key[16], existing[33];
                snprintf(key, sizeof(key), "ssid_%d", i);
                sz = sizeof(existing);
                if (nvs_get_str(h, key, existing, &sz) == ESP_OK && strcmp(existing, ssid) == 0) {
                    snprintf(key, sizeof(key), "pass_%d", i);
                    sz = pass_sz;
                    esp_err_t err = nvs_get_str(h, key, pass, &sz);
                    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
                        if (err == ESP_ERR_NVS_NOT_FOUND) pass[0] = 0;
                        ok = true;
                    }
                    break;
                }
            }
        }
        nvs_close(h);
    }
    nvs_unlock();
    return ok;
}

void wifi_sta_register_cb(wifi_sta_cb_t cb, void *user) {
    s_user_cb = cb; s_user_data = user;
}
void wifi_sta_unregister_cb(wifi_sta_cb_t cb) {
    if (s_user_cb == cb) { s_user_cb = NULL; s_user_data = NULL; }
}

void wifi_sta_stop(void) {
#if CONFIG_O_PLATFORM_SIM_MODE
    return;
#endif
    if (!s_inited) return;
    /* 手动停止属预期行为：挂起自动重连，否则 disconnect 事件会在
     * stop/deinit 进行中触发 esp_wifi_connect()，与关闭流程竞争 */
    s_suspended = true;
    if (s_connected) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    s_inited = false;
    s_connected = false;
    s_cur_ssid[0] = 0;
    ESP_LOGI(TAG, "WiFi STA 已彻底停止");
}

esp_err_t wifi_sta_connect_default(void) {
    return wifi_sta_autoconnect();
}
