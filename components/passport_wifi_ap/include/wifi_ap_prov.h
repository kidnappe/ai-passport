// main/components/passport_wifi_ap/include/wifi_ap_prov.h —— 热点配网 C 接口
// 包装小智原版 WifiConfigurationAp(softAP + HTTP captive portal)供 main.c 调用。
// 配网流程与旧 BLE 配网对等: start(开热点) -> 轮询 get_creds(手机网页提交) -> stop(关热点)。
// 连接 WiFi 由 main/wifi_sta.c 负责(wifi_sta_connect 自带 STA 模式恢复)。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 启动热点配网(softAP + HTTP server + DNS captive portal)。
// 幂等: 已在运行则直接返回 OK。
esp_err_t wifi_ap_prov_start(void);

// 停止热点配网: 关 HTTP/DNS/AP, 并恢复 STA 模式(驱动已 init 时)。
void wifi_ap_prov_stop(void);

// 取走网页提交的 WiFi 凭证(读取后标志清除, 只成功返回一次 true; 未收到返回 false)。
bool wifi_ap_prov_get_creds(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz);

// 通用 captive DNS: 供"传输页自开热点"复用——把设备 AP 网关(192.168.4.1)当所有域名解析目标,
// 让手机连上后自动弹出。配网流程不用它(配网自带 captive)。幂等。
esp_err_t wifi_ap_captive_dns_start(uint32_t gateway_ip);   // 主机序 IPv4
void      wifi_ap_captive_dns_stop(void);

#ifdef __cplusplus
}
#endif
