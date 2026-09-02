// main/ble_prov.c —— 蓝牙(BLE/NimBLE)配网：GATT 服务器接收 WiFi 凭证
// 手机用 BLE 调试工具(nRF Connect 等)连接后，向配网特征写入两行文本：
//   第1行: WiFi 名称(SSID)
//   第2行: 密码(开放网络留空)
#include "ble_prov.h"
#include "voice_ble.h"
#include "passport_ppt.h"
#include "wifi_sta.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static inline size_t strlcpy_local(char *dst, const char *src, size_t dstsize) {
    if (dstsize == 0) return strlen(src);
    size_t n = strlen(src);
    if (n >= dstsize) n = dstsize - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
    return strlen(src);
}
#define strlcpy(dst, src, dstsize) strlcpy_local(dst, src, dstsize)

static const char *TAG = "ble_prov";
/* 广播名分两套身份（语音页 vs PPT 页），避免 Windows 把语音广播误判为
 * HID 键盘：
 *   - 语音页：名 "AI Passport Voice"，只广播 0xA2B0，外观=普通外设，
 *     语音桌面端(companion)按此名精确发现设备；
 *   - PPT 页：名 "AI Passport"，广播 0xA2B0+0x1812(HID)，外观=键盘(0x03C1)，
 *     主机按此识别为输入设备并持久配对。
 * 配网页发现按扫描应答中的 128-bit 配网服务 UUID 过滤，不受名字影响。 */
static const char *DEV_NAME_VOICE = "AI Passport Voice";
static const char *DEV_NAME_PPT   = "AI Passport";

// 当前广播身份（默认 PPT，与历史行为一致）
static volatile bool s_identity_voice = false;

// 服务: 8E7F0001-2B4D-4C9A-B5C1-9E3D6F0A5B21
// 特征: 8E7F0002=SSID(写)  8E7F0003=密码(写)  8E7F0004=控制(写, 0x01 触发连接)
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x01,0x00,0x7f,0x8e);
static const ble_uuid128_t s_ssid_uuid = BLE_UUID128_INIT(
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x02,0x00,0x7f,0x8e);
static const ble_uuid128_t s_pass_uuid = BLE_UUID128_INIT(
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x03,0x00,0x7f,0x8e);
static const ble_uuid128_t s_ctrl_uuid = BLE_UUID128_INIT(
    0x21,0x5b,0x0a,0x6f,0x3d,0x9e,0xc1,0xb5,
    0x9a,0x4c,0x4d,0x2b,0x04,0x00,0x7f,0x8e);

static uint16_t s_ssid_val_h, s_pass_val_h, s_ctrl_val_h;
static volatile bool s_running, s_got;
static volatile bool s_have_ssid;           // SSID 已收到, 等密码+控制命令
static char s_ssid[33], s_pass[65];
static uint16_t s_conn_h = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;
/* 活跃连接计数:ble_gap_conn_active() 返回的是"本机是否作为 master 正在发起连接"
 * 的布尔值,不是连接数,不能用来判断空闲槽。用自维护计数决定语音页能否再留一个
 * 可连接广播槽给第二个客户端(语音桌面端)。 */
static volatile uint8_t s_conn_count;

// GATT 服务表（在 ble_gatts_count_cfg/add_svcs 中注册）
static int cb_ssid(uint16_t conn, uint16_t attr,
                   struct ble_gatt_access_ctxt *ctxt, void *arg);
static int cb_pass(uint16_t conn, uint16_t attr,
                   struct ble_gatt_access_ctxt *ctxt, void *arg);
static int cb_ctrl(uint16_t conn, uint16_t attr,
                   struct ble_gatt_access_ctxt *ctxt, void *arg);
static const struct ble_gatt_svc_def s_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY,
      .uuid = &s_svc_uuid.u,
      .characteristics = (struct ble_gatt_chr_def[]) {
          { .uuid = &s_ssid_uuid.u,
            .access_cb = cb_ssid,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            .val_handle = &s_ssid_val_h },
          { .uuid = &s_pass_uuid.u,
            .access_cb = cb_pass,
            .flags = BLE_GATT_CHR_F_WRITE,
            .val_handle = &s_pass_val_h },
          { .uuid = &s_ctrl_uuid.u,
            .access_cb = cb_ctrl,
            .flags = BLE_GATT_CHR_F_WRITE,
            .val_handle = &s_ctrl_val_h },
          { 0 } } },
    { 0 }
};
static void adv_restart(void);

static void save_creds(const char *ssid, const char *pass) {
    wifi_sta_save_creds(ssid, pass);
    ESP_LOGI(TAG, "凭证已存 NVS: %s", ssid);
}

// 通用写入: 取 mbuf 全部数据到 buf
static int write_to_buf(struct ble_gatt_access_ctxt *ctxt, char *buf, int bufsz) {
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > (uint16_t)bufsz - 1) len = (uint16_t)bufsz - 1;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, &len) != 0)
        return -1;
    buf[len] = 0;
    // 去 CR
    for (char *p = buf; *p; p++) if (*p == 13) memmove(p, p + 1, strlen(p));
    return 0;
}

static int cb_ssid(uint16_t conn, uint16_t attr,
                   struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        char info[48];
        int n = snprintf(info, sizeof(info), "ssid=%s", s_have_ssid ? s_ssid : "-");
        return os_mbuf_append(ctxt->om, info, n);
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    char buf[40] = {0};
    if (write_to_buf(ctxt, buf, sizeof(buf)) != 0) return BLE_ATT_ERR_INSUFFICIENT_RES;
    if (!buf[0]) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    strlcpy(s_ssid, buf, sizeof(s_ssid));
    s_have_ssid = true;
    ESP_LOGI(TAG, "收到 SSID: %s", s_ssid);
    return 0;
}

static int cb_pass(uint16_t conn, uint16_t attr,
                   struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return os_mbuf_append(ctxt->om, "", 0);
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    char buf[68] = {0};
    if (write_to_buf(ctxt, buf, sizeof(buf)) != 0) return BLE_ATT_ERR_INSUFFICIENT_RES;
    strlcpy(s_pass, buf, sizeof(s_pass));
    ESP_LOGI(TAG, "收到密码, 长度=%d", (int)strlen(buf));
    return 0;
}

static int cb_ctrl(uint16_t conn, uint16_t attr,
                   struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return os_mbuf_append(ctxt->om, "", 0);
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    char buf[8] = {0};
    if (write_to_buf(ctxt, buf, sizeof(buf)) != 0) return BLE_ATT_ERR_INSUFFICIENT_RES;
    if ((uint8_t)buf[0] != 0x01) {
        ESP_LOGW(TAG, "未知控制命令 0x%02x", (uint8_t)buf[0]);
        return 0;
    }
    if (!s_have_ssid) {
        ESP_LOGW(TAG, "连接命令但未收到 SSID, 忽略");
        return 0;
    }
    ESP_LOGI(TAG, "控制命令: 触发连接");
    s_got = true;
    save_creds(s_ssid, s_pass);
    return 0;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_h = event->connect.conn_handle;
            if (s_conn_count < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) s_conn_count++;
            ESP_LOGI(TAG, "设备已连接 (handle %u, 活跃 %u)", s_conn_h, s_conn_count);
            /* 连接后是否保持可连接广播,按身份区分:
             * - 语音身份:需要给第二个客户端(语音桌面端)留广播槽,只要还有空闲
             *   连接槽就重启可连接广播;
             * - PPT 身份(HID 键盘):Windows 连接后立刻进入 SMP 配对,此刻重启
             *   广播会打断配对状态机 → 配对失败。故 PPT 身份不重启广播,断开后
             *   由 DISCONNECT 分支恢复。 */
            if (s_identity_voice && s_running &&
                s_conn_count < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
                adv_restart();
            }
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "设备断开, reason=%d", event->disconnect.reason);
        if (s_conn_count > 0) s_conn_count--;
        if (s_conn_h == event->disconnect.conn.conn_handle) {
            s_conn_h = BLE_HS_CONN_HANDLE_NONE;
        }
        /* 有连接释放出空槽时恢复可连接广播(供另一客户端接入) */
        if (!s_got && s_running) adv_restart();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (!s_got && s_running) adv_restart();
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU 协商: %d", event->mtu.value);
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* 本机 NVS 里还留着 bond、对端却丢了密钥重新发配对请求(典型:Windows
         * 删除配对记录后重连)。host 默认静默忽略该请求,Windows 端表现为
         * "添加设备转圈失败"。按 NimBLE 官方示例删旧 bond,返回 RETRY 让
         * 配对流程继续(bond 随新配对重建)。 */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        return 0;
    }
}

// ---- adv_restart 定义（前向声明在文件头） ----
static void adv_restart(void) {
    const char *name = s_identity_voice ? DEV_NAME_VOICE : DEV_NAME_PPT;

    /* 身份切换要真正生效必须先停当前广播再重开:connectable 广播被连接后会自动
     * 停,但 DISCONNECT/ADV_COMPLETE/身份切换时可能仍在广播,直接 adv_start 会返回
     * EALREADY(rc=2)且不会换用新的 own_addr/name。先 stop(未广播时返回错误可忽略),
     * 保证下面按新身份重启。 */
    ble_gap_adv_stop();

    /* 地址级身份隔离:语音页用一个"从 public 派生的静态随机地址"广播,PPT 页用
     * public。原因——Windows 是按蓝牙地址(非广播内容)记住已配对键盘的,只要用同一
     * public 地址广播,Windows 就秒抢连接并把它当 HID 接管,语音桌面端(bleak,跑在
     * 同一 Windows 适配器上)再连同地址时被 HID 链路占用、GATT 只暴露 HID 缓存视图,
     * 报 "0xA2B2 not found"。语音页换独立随机地址后 Windows 认不出→不抢连,语音客户端
     * 拿到干净 GATT。派生自 public 保证每次开机稳定(便于语音端重连/记住)。 */
    uint8_t own_addr;
    if (s_identity_voice) {
        uint8_t pub[6], vrnd[6];
        if (ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, pub, NULL) == 0) {
            memcpy(vrnd, pub, 6);
            vrnd[5] |= 0xC0;         /* 高 2 bit=11 → 静态随机地址 */
            vrnd[0] ^= 0x5A;         /* 与 public 明确不同且非全0/全1 */
            ble_hs_id_set_rnd(vrnd); /* 下发到 controller + host 随机身份 */
            own_addr = BLE_OWN_ADDR_RANDOM;
        } else {
            own_addr = BLE_OWN_ADDR_PUBLIC;  /* 取不到 public 兜底 */
        }
    } else {
        own_addr = s_own_addr_type;          /* PPT/配网:public 身份,Windows 稳定配键盘 */
    }

    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (const uint8_t *)name;
    f.name_len = strlen(name);
    f.name_is_complete = 1;
    /* 16-bit 服务 UUID 放广播包。语音页只广播 0xA2B0（不带 HID 0x1812、
     * 外观=普通外设），Windows 才不会把它误判为键盘；PPT 页广播
     * 0xA2B0 + 0x1812、外观=键盘，主机按此识别为输入设备并持久配对。
     * 配网 128-bit UUID 在扫描应答(下方)。 */
    static const ble_uuid16_t s_voice_svc16 = BLE_UUID16_INIT(AUDIO_SVC_UUID);
    static const ble_uuid16_t s_hid_svc16   = BLE_UUID16_INIT(0x1812);
    static const ble_uuid16_t s_uuids_voice[1] = { s_voice_svc16 };
    static const ble_uuid16_t s_uuids_ppt[2]   = { s_voice_svc16, s_hid_svc16 };
    if (s_identity_voice) {
        f.uuids16 = s_uuids_voice;
        f.num_uuids16 = 1;
        f.appearance = 0x0000;          // 普通外设（非键盘）
    } else {
        f.uuids16 = s_uuids_ppt;
        f.num_uuids16 = 2;
        f.appearance = 0x03C1;          // 键盘 (HID Appearance)
    }
    f.uuids16_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) { ESP_LOGE(TAG, "adv fields 失败: %d", rc); return; }

    struct ble_hs_adv_fields r;
    memset(&r, 0, sizeof(r));
    r.uuids128 = &s_svc_uuid;
    r.num_uuids128 = 1;
    r.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&r);
    if (rc != 0) { ESP_LOGE(TAG, "adv rsp 失败: %d", rc); return; }

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr, NULL, BLE_HS_FOREVER, &p, gap_event_cb, NULL);
    if (rc != 0) ESP_LOGE(TAG, "广播启动失败: %d", rc);
    else ESP_LOGI(TAG, "广播中: %s (堆余 %lu KB)", name,
        (unsigned long)esp_get_free_heap_size() / 1024);
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    adv_restart();
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "BLE 复位: %d", reason);
}

static void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_prov_start(void) {
#if CONFIG_O_PLATFORM_SIM_MODE
    /* QEMU 不支持 BLE 控制器, 配网页会显示"配网失败或已取消" */
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (s_running) {
        /* 栈常开(语音通道需要):已在跑则只重启配网广播 */
        adv_restart();
        return ESP_OK;
    }
    s_got = false;
    s_have_ssid = false;
    s_ssid[0] = 0;
    s_pass[0] = 0;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "nimble init 失败: %s", esp_err_to_name(err)); return err; }

    /* GAP 设备名跟随广播身份（语音页/PPT 页不同名），esp_hid 内部也会
     * 用 "PPT-Remote" 覆盖 GAP 名，但广播字段里的名字由 adv_restart 独立
     * 设置，桌面端按广播名发现，不受 GAP 名影响。 */
    int rc = ble_svc_gap_device_name_set(
        s_identity_voice ? DEV_NAME_VOICE : DEV_NAME_PPT);
    if (rc != 0) ESP_LOGE(TAG, "name set 失败: %d", rc);

    /* HID 键盘服务(0x1812,PPT 遥控器):用官方 esp_hid 组件(NimBLE 后端)注册。
     * 必须在其它 count_cfg/add_svcs 之前调用——esp_hid 内部会注册标准
     * GAP/GATT/BAS/DIS/HID 服务(手写 NimBLE GATT 在 Windows 上认不成键盘,
     * 见 passport_ppt 组件注释)。成功则 GAP/GATT 已由它注册;失败降级
     * (PPT 不可用),不影响配网/语音。 */
    esp_err_t herr = passport_ppt_register();
    if (herr != ESP_OK) {
        ESP_LOGW(TAG, "HID 服务注册失败: %s", esp_err_to_name(herr));
        /* esp_hid 初始化失败时,GAP/GATT 标准服务未被注册,补注册以免
         * 配网/语音的服务发现出问题。 */
        ble_svc_gap_init();
        ble_svc_gatt_init();
    }

    rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) ESP_LOGE(TAG, "count_cfg 失败: %d", rc);
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) ESP_LOGE(TAG, "add_svcs 失败: %d", rc);

    /* 语音 GATT 服务(0xA2B0)与配网服务一起在 host 启动前注册进 GATT 表。
     * 失败只降级(语音不可用),不影响配网。 */
    esp_err_t verr = voice_ble_register();
    if (verr != ESP_OK) ESP_LOGW(TAG, "语音服务注册失败: %s", esp_err_to_name(verr));

    /* sync_cb/reset_cb 归本文件管理(负责启动广播)。esp_hid 初始化时设置的
     * sync_cb(reset_cb)被覆盖:广播由本文件的 on_sync 负责,esp_hid 的
     * START 事件仅用于示例里手动起广播,本平台不需要。esp_hid 的
     * gatts_register_cb 保留(捕获 report handle,发按键依赖它)。 */
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_freertos_init(host_task);
    s_running = true;
    s_conn_count = 0;
    s_conn_h = BLE_HS_CONN_HANDLE_NONE;
    ESP_LOGI(TAG, "BLE 栈启动(配网+语音), 广播 %s",
             s_identity_voice ? DEV_NAME_VOICE : DEV_NAME_PPT);
    return ESP_OK;
}

void ble_prov_set_identity(bool is_voice) {
    s_identity_voice = is_voice;
    /* 栈已跑则立即按新身份重启广播；未跑则仅记录，ble_prov_start 时生效。
     * 栈运行期间不能改 GAP 设备名（esp_hid 已固定），但广播字段可随时
     * 重设，桌面端按广播名发现，故这里只需 adv_restart。 */
    if (s_running) {
        adv_restart();
    }
}

esp_err_t ble_prov_stop(void) {
    if (!s_running) return ESP_OK;
    /* 配网结束但栈保留:语音通道(passport_voice)需要 BLE 常驻。
     * 只断开配网连接并重启广播(语音+配网同播),不反初始化 NimBLE。 */
    if (s_conn_h != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_h, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(200));
        s_conn_h = BLE_HS_CONN_HANDLE_NONE;
    }
    adv_restart();
    ESP_LOGI(TAG, "配网结束,BLE 栈保留(语音通道), 堆余 %lu KB",
        (unsigned long)esp_get_free_heap_size() / 1024);
    return ESP_OK;
}

esp_err_t ble_stack_stop(void) {
    /* 设置里关闭蓝牙开关:完整反初始化 NimBLE,语音通道随之关闭。 */
    if (!s_running) return ESP_OK;
    s_running = false;
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    if (s_conn_h != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_h, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    /* HID 服务用 esp_hid 组件,其内部静态状态(esp_hidd_dev_t)在 NimBLE
     * 反初始化后仍残留;必须先卸载,下次 ble_prov_start 才能重新注册。 */
    passport_ppt_deinit();
    int rc = nimble_port_stop();
    if (rc == 0) nimble_port_deinit();
    s_conn_h = BLE_HS_CONN_HANDLE_NONE;
    s_conn_count = 0;
    ESP_LOGI(TAG, "BLE 栈停止(蓝牙开关关), 堆余 %lu KB",
        (unsigned long)esp_get_free_heap_size() / 1024);
    return ESP_OK;
}

bool ble_prov_is_running(void) { return s_running; }

bool ble_prov_get_creds(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz) {
    if (!s_got) return false;
    strlcpy(ssid, s_ssid, ssid_sz);
    strlcpy(pass, s_pass, pass_sz);
    s_got = false;
    return true;
}
