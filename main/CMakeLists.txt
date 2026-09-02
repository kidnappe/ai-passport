idf_component_register(
    SRCS "main.c" "ble_prov.c" "wifi_sta.c" "transfer_page.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bsp passport_core passport_ui human_display passport_voice passport_wifi_ap passport_ppt nvs_flash
    REQUIRES esp_wifi bt esp_http_client esp_http_server
    EMBED_TXTFILES "transfer_editor.html"
)
