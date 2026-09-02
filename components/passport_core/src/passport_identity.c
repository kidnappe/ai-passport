#include "passport_identity.h"

#include "esp_log.h"
#include "esp_mac.h"
#include <stdbool.h>
#include <stdio.h>

static const char *TAG = "passport_identity";

static uint64_t s_device_id;
static char s_device_code[PASSPORT_DEVICE_CODE_MAX];
static bool s_ready;

void passport_identity_format(uint64_t device_id, char out[PASSPORT_DEVICE_CODE_MAX])
{
    /* 48-bit MAC as 12-char hex lowercase, matching official format */
    snprintf(out, PASSPORT_DEVICE_CODE_MAX, "%012llx", (unsigned long long)device_id);
}

esp_err_t passport_identity_parse_code(const char *code, uint64_t *out_id)
{
    if (!code || !out_id) return ESP_ERR_INVALID_ARG;
    char buf[13];
    size_t n = 0;
    for (const char *p = code; *p; ++p) {
        if (*p == '-' || *p == ':') continue;
        if (n >= sizeof(buf) - 1) return ESP_ERR_INVALID_ARG;
        buf[n++] = *p;
    }
    buf[n] = '\0';
    if (n != 12) return ESP_ERR_INVALID_ARG;

    char *end = NULL;
    uint64_t value = strtoull(buf, &end, 16);
    if (end != buf + n || value > 0xFFFFFFFFFFFFULL) {
        return ESP_ERR_INVALID_CRC;
    }
    *out_id = value;
    return ESP_OK;
}

esp_err_t passport_identity_init(void)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取工厂 MAC 失败: %s", esp_err_to_name(err));
        return err;
    }

    s_device_id = 0;
    for (size_t i = 0; i < sizeof(mac); ++i) {
        s_device_id = (s_device_id << 8) | mac[i];
    }
    passport_identity_format(s_device_id, s_device_code);
    s_ready = true;
    ESP_LOGI(TAG, "设备码: %s", s_device_code);
    return ESP_OK;
}

uint64_t passport_identity_id(void)
{
    return s_ready ? s_device_id : 0;
}

const char *passport_identity_code(void)
{
    return s_ready ? s_device_code : "未初始化";
}
