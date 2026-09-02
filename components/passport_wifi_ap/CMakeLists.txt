# components/passport_wifi_ap —— 小智原版热点配网（softAP + HTTP captive portal）
# 移植自 78/esp-wifi-connect v3.2.2（folo-ai-passport-xiaozhi-main managed_components），
# 只保留 AP 配网相关文件（wifi_configuration_ap / dns_server / ssid_manager），
# 砍掉 wifi_manager / wifi_station（STA 连接由 main/wifi_sta.c 负责）。
# 凭证存储：NVS 命名空间 "wifi"（与 wifi_sta.c 的 "wifi_sta" 互不冲突）。
idf_component_register(
    SRCS
        "wifi_configuration_ap.cc"
        "ssid_manager.cc"
        "dns_server.cc"
        "wifi_ap_prov.cc"
    INCLUDE_DIRS
        "include"
    EMBED_TXTFILES
        "assets/wifi_configuration.html"
        "assets/wifi_configuration_done.html"
    REQUIRES
        "esp_timer"
        "esp_http_server"
        "esp_wifi"
        "nvs_flash"
        "json"
)
