#include "transfer_page.h"
#include "passport_ui.h"
#include "passport_storage.h"
#include "passport_identity.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/err.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* 内嵌的设备端编辑页（main/CMakeLists.txt EMBED_TXTFILES 注入） */
extern const char transfer_editor_html_start[] asm("_binary_transfer_editor_html_start");
extern const char transfer_editor_html_end[]   asm("_binary_transfer_editor_html_end");

static const char *TAG = "transfer";

static void set_dyn_flag(bool dyn) {   /* 与 main.c 同一命名空间 pass_net / key "dyn" */
    nvs_handle_t h;
    if (nvs_open("pass_net", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "dyn", dyn ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static passport_page_t *s_page;
static lv_obj_t *s_status;
static lv_obj_t *s_url_label;
static httpd_handle_t s_server;
static bool s_server_running;

/* ---- B2: 传输页自开一个"免密热点"(APSTA, WIFI_AUTH_OPEN)，手机连上后开浏览器到 192.168.4.1 ---- */
static esp_netif_t *s_ap_netif;
static bool s_ap_open;

static void build_ap_ssid(char *out, size_t n)
{
    snprintf(out, n, "Passport-Set-%s", passport_identity_code());
}

static esp_err_t open_softap(void)
{
    if (s_ap_open) return ESP_OK;
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_config_t wc = {0};
    char ssid[40];
    build_ap_ssid(ssid, sizeof(ssid));
    strncpy((char *)wc.ap.ssid, ssid, sizeof(wc.ap.ssid) - 1);
    wc.ap.ssid_len = strlen(ssid);
    wc.ap.channel = 1;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_OPEN;   /* 免密，和配网热点一致 */
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (err == ESP_OK) s_ap_open = true;
    return err;
}

static void close_softap(void)
{
    if (!s_ap_open) return;
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_open = false;
}

static esp_err_t handle_ping(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "pong");
    return ESP_OK;
}

/* 根路径直接返回内嵌编辑页：手机/电脑连上设备热点后开 http://192.168.4.1/ 即可 */
static esp_err_t handle_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, transfer_editor_html_start,
                    transfer_editor_html_end - transfer_editor_html_start);
    return ESP_OK;
}

/* 诊断接口：返回当前存储的字段值，用于远程核查文件内容 */
static esp_err_t handle_get_fields(httpd_req_t *req)
{
    static const char *const names[] = {"nickname", "college", "major", "student_id"};
    char body[1024];
    size_t off = 0;
    off += snprintf(body + off, sizeof(body) - off, "{");
    for (size_t i = 0; i < 4; ++i) {
        char buf[64] = {0};
        size_t len = 0;
        char shown[70] = "";
        char path[64];
        snprintf(path, sizeof(path), PASSPORT_FS_ROOT "/%s.txt", names[i]);
        if (passport_storage_read_text(path, buf, sizeof(buf), &len) == ESP_OK && len > 0) {
            snprintf(shown, sizeof(shown), "%s", buf);
        }
        off += snprintf(body + off, sizeof(body) - off,
                        "%s\"%s\":\"%s\"", i ? "," : "", names[i], shown);
        if (off >= sizeof(body) - 2) break;
        /* 自定义标签（第二类行）一并回传，供编辑页回填 */
        if (i != 0) {
            char lb[24] = {0}; size_t ll = 0; char lpath[64];
            snprintf(lpath, sizeof(lpath), PASSPORT_FS_ROOT "/%s_label.txt", names[i]);
            if (passport_storage_read_text(lpath, lb, sizeof(lb), &ll) == ESP_OK && ll > 0) {
                char lshow[30] = ""; snprintf(lshow, sizeof(lshow), "%s", lb);
                off += snprintf(body + off, sizeof(body) - off, ",\"%s_label\":\"%s\"", names[i], lshow);
                if (off >= sizeof(body) - 2) break;
            }
        }
    }
    if (off > sizeof(body) - 2) off = sizeof(body) - 2;
    snprintf(body + off, sizeof(body) - off, "}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

static void save_field(const char *path, const char *data, size_t len)
{
    if (!data || len == 0) return;
    /* multipart 空行保险：值不应以换行开头 */
    while (len > 0 && (*data == '\r' || *data == '\n')) { data++; len--; }
    if (len == 0) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    fwrite(data, 1, len, f);
    fclose(f);
    ESP_LOGI(TAG, "Saved %s: %.*s", path, (int)len, data);
}

static esp_err_t handle_post_upload(httpd_req_t *req)
{
    char buf[1024];
    char boundary[64] = {0};
    int remaining = req->content_len;

    int ret = httpd_req_get_hdr_value_str(req, "Content-Type", buf, sizeof(buf) - 1);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing Content-Type header");
        return ESP_FAIL;
    }

    const char *bstart = strstr(buf, "boundary=");
    if (!bstart) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No boundary in Content-Type");
        return ESP_FAIL;
    }
    snprintf(boundary, sizeof(boundary), "--%s", bstart + 9);

    char field_name[64] = {0};
    char field_value[256] = {0};
    size_t val_len = 0;
    bool in_field = false;
    bool in_file = false;
    FILE *avatar_file = NULL;
    size_t filled = 0;

    while (remaining > 0 || filled > 0) {
        if (remaining > 0 && filled < sizeof(buf) - 1) {
            int cap = (int)sizeof(buf) - 1 - (int)filled;
            int read_len = remaining < cap ? remaining : cap;
            ret = httpd_req_recv(req, buf + filled, read_len);
            if (ret <= 0) break;
            filled += (size_t)ret;
            buf[filled] = 0;
        }

        bool need_more = false;
        char *pos = buf;
        while (pos < buf + filled) {
            if (in_field) {
                char *end = strstr(pos, boundary);
                if (end) {
                    size_t copy = end - pos;
                    if (copy > 2 && pos[copy - 2] == '\r' && pos[copy - 1] == '\n') copy -= 2;
                    if (copy > 0 && val_len + copy < sizeof(field_value) - 1) {
                        memcpy(field_value + val_len, pos, copy);
                        val_len += copy;
                    }
                    if (strlen(field_name) > 0) {
                        char path[128];
                        snprintf(path, sizeof(path), PASSPORT_FS_ROOT "/%s.txt", field_name);
                        save_field(path, field_value, val_len);
                    }
                    in_field = false;
                    field_name[0] = 0;
                    val_len = 0;
                    pos = end;
                } else if (remaining > 0) {
                    /* 值未结束：已全部暂存到 field_value，缓冲区清空等下一块 */
                    need_more = true;
                    break;
                } else {
                    pos = buf + filled;
                }
            } else if (in_file) {
                char *end = strstr(pos, boundary);
                if (end) {
                    size_t write_len = end - pos;
                    if (write_len > 2 && pos[write_len - 2] == '\r' && pos[write_len - 1] == '\n')
                        write_len -= 2;
                    if (write_len > 0 && avatar_file)
                        fwrite(pos, 1, write_len, avatar_file);
                    in_file = false;
                    if (avatar_file) { fclose(avatar_file); avatar_file = NULL; }
                    pos = end;
                } else if (remaining > 0) {
                    if (avatar_file)
                        fwrite(pos, 1, filled - (size_t)(pos - buf), avatar_file);
                    need_more = true;
                    break;
                } else {
                    if (avatar_file)
                        fwrite(pos, 1, filled - (size_t)(pos - buf), avatar_file);
                    pos = buf + filled;
                }
            } else {
                char *nl = strstr(pos, "\r\n");
                if (!nl) {
                    if (remaining > 0) {
                        /* 半行留在缓冲区头部，与下一块拼接后再解析 */
                        need_more = true;
                    } else {
                        pos = buf + filled;
                    }
                    break;
                }
                *nl = 0;
                if (strstr(pos, "name=\"")) {
                    char *nstart = strstr(pos, "name=\"") + 6;
                    char *nend = strchr(nstart, '"');
                    if (nend) {
                        *nend = 0;
                        size_t nlen = nend - nstart;
                        if (nlen < sizeof(field_name)) {
                            memcpy(field_name, nstart, nlen);
                            field_name[nlen] = 0;
                        }
                        if (strstr(pos, "filename=\"")) {
                            in_file = true;
                            if (strstr(field_name, "avatar")) {
                                avatar_file = fopen(PASSPORT_FS_ROOT "/avatar.raw", "wb");
                            }
                        } else {
                            in_field = true;
                            val_len = 0;
                        }
                    }
                }
                *nl = '\r';
                pos = nl + 2;
                /* part 头之后有一个空行（\r\n），必须跳过——否则空行会被写进
                 * 值/头像开头：文本被推到下一行、头像尺寸校验永远失败 */
                if ((in_field || in_file) &&
                    pos + 1 < buf + filled && pos[0] == '\r' && pos[1] == '\n') {
                    pos += 2;
                }
            }
        }

        if (need_more && (in_field || in_file)) {
            /* 值/文件流已全部消费，清空缓冲区等下一块 */
            filled = 0;
        } else {
            /* 把未解析的尾部（半行）压回缓冲区头 */
            filled -= (size_t)(pos - buf);
            memmove(buf, pos, filled);
        }
        buf[filled] = 0;
        if (need_more && remaining <= 0) break;
    }

    /* 编辑页会带一个 avatar_mode 字段（photo/pet）：据此切 NVS dyn，并删除临时文件 */
    {
        char am[8] = {0}; size_t al = 0;
        char amp[64]; snprintf(amp, sizeof(amp), PASSPORT_FS_ROOT "/avatar_mode.txt");
        if (passport_storage_read_text(amp, am, sizeof(am), &al) == ESP_OK && al > 0) {
            set_dyn_flag(strncmp(am, "pet", 3) == 0);   /* pet→动态(dyn=1)，其余含 photo→静态 */
            nvs_handle_t h;  /* 用 storage 层删除该临时文件 */
            (void)h;
            remove(amp);
        }
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "OK");
    ESP_LOGI(TAG, "Upload complete");

    /* httpd 线程无权直接碰 LVGL：必须持锁，否则与渲染任务并发会刷死在重绘链表 */
    if (s_status && bsp_lvgl_lock(1000)) {
        passport_ui_label_set_text(s_status, "传输完成，返回主页查看");
        bsp_lvgl_unlock();
    }
    return ESP_OK;
}

static void start_http_server(void)
{
    if (s_server) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 8;
    config.stack_size = 4096;
    config.server_port = 80;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        if (s_status) passport_ui_label_set_text(s_status, "HTTP 服务器启动失败");
        return;
    }

    httpd_register_uri_handler(s_server, &(httpd_uri_t){
        .uri = "/", .method = HTTP_GET, .handler = handle_index
    });
    httpd_register_uri_handler(s_server, &(httpd_uri_t){
        .uri = "/ping", .method = HTTP_GET, .handler = handle_ping
    });
    httpd_register_uri_handler(s_server, &(httpd_uri_t){
        .uri = "/fields", .method = HTTP_GET, .handler = handle_get_fields
    });
    httpd_register_uri_handler(s_server, &(httpd_uri_t){
        .uri = "/upload", .method = HTTP_POST, .handler = handle_post_upload
    });

    s_server_running = true;
    ESP_LOGI(TAG, "HTTP server started on port 80");
}

static void stop_http_server(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        s_server_running = false;
    }
}


void show_transfer_wifi(void)
{
    /* 开免密热点 + 起 HTTP 服务(设备自己 serve 编辑页)；手机连热点后开 http://192.168.4.1/ */
    if (open_softap() != ESP_OK) {
        if (s_status) passport_ui_label_set_text(s_status, "热点启动失败");
        return;
    }
    if (!s_server_running) start_http_server();

    char ssid[40];
    build_ap_ssid(ssid, sizeof(ssid));
    char line[96];
    snprintf(line, sizeof(line), "热点: %s（无密码）", ssid);
    if (s_status) passport_ui_label_set_text(s_status, line);
    if (s_url_label) passport_ui_label_set_text(s_url_label, "手机连上后浏览器打开  http://192.168.4.1/");
}

void show_transfer(void)
{
    if (s_page) return;

    s_page = passport_ui_page_create("传输", true, true);
    s_status = passport_ui_label_create(s_page, "编辑工牌字段/头像");
    s_url_label = passport_ui_label_create(s_page, "按 OK 开启传输热点");
    passport_ui_label_create(s_page, "");
    passport_ui_label_create(s_page, "1) 手机连热点 Passport-Set-xxxx(无密码)");
    passport_ui_label_create(s_page, "2) 浏览器打开 http://192.168.4.1/");
    passport_ui_page_set_actions(s_page, "开启热点", "主页");
    passport_ui_page_show(s_page);
}

void transfer_stop(void)
{
    close_softap();
    stop_http_server();
    /* s_page must be cleared too, otherwise show_transfer() early-returns on
     * the next visit and leaves the user on a blank screen. */
    passport_ui_page_destroy(s_page);
    s_page = NULL;
    s_status = NULL;
    s_url_label = NULL;
}