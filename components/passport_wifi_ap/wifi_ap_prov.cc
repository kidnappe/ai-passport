// components/passport_wifi_ap/wifi_ap_prov.cc —— 热点配网 C 接口实现
// 全局单例包装 WifiConfigurationAp, 配网凭证经 OnCredentials 回调存入静态缓冲,
// main 侧 wifi_prov_task 轮询 wifi_ap_prov_get_creds() 取走。
#include "wifi_ap_prov.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_wifi.h>

#include "wifi_configuration_ap.h"
#include "dns_server.h"

#define TAG "wifi_ap_prov"
#define PROV_SSID_PREFIX "AI Passport"

static std::unique_ptr<WifiConfigurationAp> s_ap;
static std::unique_ptr<DnsServer> s_captive_dns;   /* 通用 captive（传输页自开热点用） */

static std::mutex s_creds_mutex;
static char s_creds_ssid[33];
static char s_creds_pass[65];
static bool s_creds_ready = false;

bool wifi_ap_prov_get_creds(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    std::lock_guard<std::mutex> lock(s_creds_mutex);
    if (!s_creds_ready) return false;
    strlcpy(ssid, s_creds_ssid, ssid_sz);
    strlcpy(pass, s_creds_pass, pass_sz);
    s_creds_ready = false;
    ESP_LOGI(TAG, "取走配网凭证: %s", s_creds_ssid);
    return true;
}

esp_err_t wifi_ap_prov_start(void)
{
    if (s_ap) {
        ESP_LOGW(TAG, "热点配网已在运行");
        return ESP_OK;
    }

    /* WiFi 驱动可能在 STA 运行中(wifi_sta_init 已 start)。
     * 原版 StartAccessPoint 直接 esp_wifi_set_mode(APSTA) + start, 驱动运行中
     * 切换模式会失败并触发 ESP_ERROR_CHECK abort —— 先停驱动再开 AP。 */
    esp_err_t st = esp_wifi_stop();
    if (st != ESP_OK && st != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "配网前停 WiFi 驱动: %s", esp_err_to_name(st));
    }

    {
        std::lock_guard<std::mutex> lock(s_creds_mutex);
        s_creds_ready = false;
        s_creds_ssid[0] = '\0';
        s_creds_pass[0] = '\0';
    }

    s_ap = std::make_unique<WifiConfigurationAp>();
    s_ap->SetSsidPrefix(PROV_SSID_PREFIX);
    s_ap->SetLanguage("zh-CN");
    s_ap->OnCredentials([](const std::string &ssid, const std::string &pass) {
        std::lock_guard<std::mutex> lock(s_creds_mutex);
        strlcpy(s_creds_ssid, ssid.c_str(), sizeof(s_creds_ssid));
        strlcpy(s_creds_pass, pass.c_str(), sizeof(s_creds_pass));
        s_creds_ready = true;
        ESP_LOGI(TAG, "收到配网凭证: %s", s_creds_ssid);
    });
    /* /exit 端点(配网页"退出配置")由用户主动触发 —— 置标志, main 侧轮询处理 */
    s_ap->OnExitRequested([]() {
        ESP_LOGI(TAG, "网页请求退出配网");
    });

    s_ap->Start();
    ESP_LOGI(TAG, "热点配网已启动, SSID: %s, 访问 http://192.168.4.1", s_ap->GetSsid().c_str());
    return ESP_OK;
}

void wifi_ap_prov_stop(void)
{
    if (s_ap) {
        s_ap->Stop();
        s_ap.reset();
    }
    {
        std::lock_guard<std::mutex> lock(s_creds_mutex);
        s_creds_ready = false;
    }

    /* 原版 Stop() 只 esp_wifi_stop() 不 deinit, 而 wifi_sta_init 的 s_inited
     * 仍为 true(不会重新 start) —— 这里显式恢复 STA 模式并重新启动驱动,
     * 之后 main 侧 wifi_sta_connect 才能正常 set_config + connect。 */
    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e == ESP_OK) {
        e = esp_wifi_start();
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "配网结束恢复 STA 失败: %s", esp_err_to_name(e));
    } else {
        ESP_LOGI(TAG, "热点配网已停止, 已恢复 STA 模式");
    }
}

esp_err_t wifi_ap_captive_dns_start(uint32_t gateway_ip)
{
    if (s_captive_dns) return ESP_OK;
    s_captive_dns = std::make_unique<DnsServer>();
    esp_ip4_addr_t gw;
    gw.addr = gateway_ip;                 /* 主机序 IPv4 (如 192.168.4.1) */
    s_captive_dns->Start(gw);
    ESP_LOGI(TAG, "captive DNS 已启动 (→ " IPSTR ")", IP2STR(&gw));
    return ESP_OK;
}

void wifi_ap_captive_dns_stop(void)
{
    if (!s_captive_dns) return;
    s_captive_dns->Stop();
    s_captive_dns.reset();
    ESP_LOGI(TAG, "captive DNS 已停止");
}
