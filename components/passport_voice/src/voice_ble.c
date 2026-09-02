// components/passport_link/src/voice_ble.c
// 语音 GATT 传输(移植自 folo-ai-passport-voice main/ble_audio.c)。
// 上:分片打包纯函数(零全局状态、零分配;任何参数非法只返回错误码,绝不越界、绝不崩溃)。
// 下(仅 ESP_PLATFORM 编译,宿主机测试跳过):NimBLE 0xA2B0 服务
//    (CTRL 0xA2B1 / EVENT 0xA2B2 / AUDIO 0xA2B3)、AUDIO/EVENT notify
//    (迭代分片 + mbuf 池背压)、CTRL 写回调(交还上层解析,零阻塞)。
//    不拥有 NimBLE 生命周期:主机启动/广播由 passport_link_ble.c 编排;
//    连接建立后请求 2M PHY + 快连接参数 + 外设侧 MTU 交换(源仓库吞吐工程)。
#include "voice_ble.h"
#include <stdbool.h>
#include <string.h>

// ==================== 分片打包纯函数(契约见 voice_ble.h) ====================

// 参数合法性:MTU 不低于规范下限 23,帧非空。
static bool frame_valid(size_t frame_len, uint16_t mtu) {
    return mtu >= ATT_MTU_MIN && frame_len > 0;
}

size_t voice_ble_chunk_count(size_t frame_len, uint16_t mtu) {
    if (!frame_valid(frame_len, mtu)) return 0;
    size_t chunk = (size_t)mtu - PAYLOAD_OVERHEAD;
    // 除余方式求 ceil,避免 frame_len 接近 SIZE_MAX 时 (frame_len+chunk-1) 溢出
    return frame_len / chunk + (frame_len % chunk != 0);
}

size_t voice_ble_chunk_len(size_t frame_len, uint16_t mtu, size_t idx) {
    if (!frame_valid(frame_len, mtu)) return 0;
    size_t chunk = (size_t)mtu - PAYLOAD_OVERHEAD;
    // 越界预判用除法(避免 idx*chunk 在超大 idx 时乘法回绕溢出后误判未越界)
    size_t count = frame_len / chunk + (frame_len % chunk != 0);
    if (idx >= count) return 0;
    size_t off = idx * chunk;
    size_t remain = frame_len - off;
    return remain < chunk ? remain : chunk;
}

voice_ble_err_t voice_ble_pack_audio(voice_ble_chunk_t *chunks, size_t chunks_cap,
                                     const uint8_t *frame, size_t frame_len, uint16_t mtu,
                                     size_t *chunk_count) {
    if (!chunks || !chunk_count || !frame) return VOICE_BLE_ERR_NULL;
    if (mtu < ATT_MTU_MIN) return VOICE_BLE_ERR_MTU;
    if (frame_len == 0) return VOICE_BLE_ERR_EMPTY;
    size_t count = voice_ble_chunk_count(frame_len, mtu);
    if (count > chunks_cap) return VOICE_BLE_ERR_SMALL_CAP;
    size_t chunk = (size_t)mtu - PAYLOAD_OVERHEAD;
    for (size_t i = 0; i < count; i++) {
        chunks[i].data = frame + i * chunk;
        chunks[i].len  = voice_ble_chunk_len(frame_len, mtu, i);
    }
    *chunk_count = count;
    return VOICE_BLE_OK;
}

voice_ble_err_t voice_ble_pack_init(voice_ble_packer_t *p, const uint8_t *frame,
                                    size_t frame_len, uint16_t mtu) {
    if (!p || !frame) return VOICE_BLE_ERR_NULL;
    if (mtu < ATT_MTU_MIN) return VOICE_BLE_ERR_MTU;
    if (frame_len == 0) return VOICE_BLE_ERR_EMPTY;
    p->frame = frame;
    p->frame_len = frame_len;
    p->offset = 0;
    p->mtu = mtu;
    return VOICE_BLE_OK;
}

voice_ble_err_t voice_ble_pack_next(voice_ble_packer_t *p, const uint8_t **chunk,
                                    size_t *chunk_len) {
    if (!p || !chunk || !chunk_len) return VOICE_BLE_ERR_NULL;
    if (!p->frame) return VOICE_BLE_ERR_NULL;          // 未初始化兜底
    if (p->mtu < ATT_MTU_MIN) return VOICE_BLE_ERR_MTU;
    if (p->offset >= p->frame_len) return VOICE_BLE_DONE;
    size_t chunk_size = (size_t)p->mtu - PAYLOAD_OVERHEAD;
    size_t remain = p->frame_len - p->offset;
    size_t n = remain < chunk_size ? remain : chunk_size;
    *chunk = p->frame + p->offset;
    *chunk_len = n;
    p->offset += n;
    return VOICE_BLE_OK;
}

voice_ble_err_t voice_ble_event_chunks(voice_ble_chunk_t *chunks, size_t chunks_cap,
                                       const char *line, size_t line_len, uint16_t mtu,
                                       size_t *chunk_count) {
    if (!chunks || !chunk_count || !line) return VOICE_BLE_ERR_NULL;
    if (mtu < ATT_MTU_MIN) return VOICE_BLE_ERR_MTU;
    if (line_len == 0) return VOICE_BLE_ERR_EMPTY;
    if (line_len > EVENT_LINE_MAX) return VOICE_BLE_ERR_TOO_LONG;
    return voice_ble_pack_audio(chunks, chunks_cap, (const uint8_t *)line, line_len, mtu,
                                chunk_count);
}

// ==================== NimBLE 外设(GATT 0xA2B0) ====================
// ESP_PLATFORM 由 ESP-IDF 构建系统自动定义;宿主机测试(无 IDF)不编译此段。

#ifdef ESP_PLATFORM

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"  // ble_hs_util_ensure_addr
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"  // IDF 5.5: store_config.h 改名

static const char *TAG = "link_voice";

#define TX_CHUNK_BUDGET_MS  50   // 单片重试预算(mbuf 池排空需要连接事件,2M 下 ~几 ms;超预算才丢片)
#define TX_RETRY_DELAY_MS   2    // mbuf 耗尽后的重试间隔(让 host 任务把已排队的片发出去)
#define TX_MUTEX_TIMEOUT_MS 100  // 发送互斥等待上限(另一发送者单片 ≤50ms,此值留双倍余量)

// 服务 128-bit:0000A2B0-0000-1000-8000-00805F9B34FB(BLE_UUID128_INIT 为小端字节序,
// 写法仿旧 ble_provisioning 的 prov_svc_uuid)
static const ble_uuid128_t s_audio_svc_uuid = BLE_UUID128_INIT(
    0xB0, 0xA2, 0x00, 0x00,   // 0000A2B0
    0x00, 0x00,               // -0000-
    0x00, 0x10,               // -1000-
    0x00, 0x80,               // -8000-
    0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB);  // -00805F9B34FB

static uint16_t s_ctrl_val_handle;
static uint16_t s_event_val_handle;
static uint16_t s_audio_val_handle;
static char s_ctrl_buf[CTRL_PAYLOAD_MAX + 1];   // 写回调暂存(静态 2KB,零堆)

// 下行与链路状态回调(passport_voice 注册;host task 上下文,只做非阻塞投递)
static voice_ble_ctrl_cb_t s_ctrl_cb;
static void *s_ctrl_user;
static voice_ble_state_cb_t s_state_cb;
static void *s_state_user;

// 连接与订阅状态(host task 写,发送任务读;volatile 足够)
static volatile uint16_t s_conn = 0xFFFF;          // 当前连接
static volatile uint16_t s_audio_conn = 0xFFFF;    // 订阅了 AUDIO CCCD 的连接
static volatile bool s_event_subscribed = false;   // EVENT CCCD 已订阅(链路通,link_up 依据)
static volatile uint32_t s_drop_audio = 0;         // 音频 notify 失败丢弃累计
static volatile uint32_t s_drop_event = 0;         // 事件行 notify 失败丢弃累计
// 掉帧计数跨任务(音频 worker / event worker / app task)++:单核下读-改-写可被
// tick 抢占打断(ISR 返回后切换任务)→ 丢递增。临界区(关中断)微秒级,不阻塞
// 实时路径;对齐 audio_streamer s_drop_mux 模式(审查 P2-5)。
static portMUX_TYPE s_drop_mux = portMUX_INITIALIZER_UNLOCKED;

static void drop_count_inc(volatile uint32_t *c)
{
    portENTER_CRITICAL(&s_drop_mux);
    (*c)++;
    portEXIT_CRITICAL(&s_drop_mux);
}

// 发送串行化:发送者有两个(audio worker 的音频帧、event_worker 的事件行),
// 用互斥锁保证一帧的分片连续发出,不被另一通道插片。
//
// ⚠ 不要用 BLE_GAP_EVENT_NOTIFY_TX 做流控。ble_gatts_notify_custom() 在返回前
// 就同步派发了该事件(IDF 5.5 ble_gattc.c),语义是"已尝试发送",不是"已上空口";
// 等它必然立刻满足,是空转。真正的在途上限是 msys mbuf 池
// (CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT × BLOCK_SIZE):池空时
// ble_hs_mbuf_from_flat() 返回 NULL —— 这才是唯一可靠的背压信号,见下方重试。
static SemaphoreHandle_t s_tx_mutex;
static volatile uint16_t s_tx_status = 0;   // 最近一次 NOTIFY_TX 的 status(仅诊断)
static struct ble_gap_event_listener s_gap_listener;  // 全局 GAP 监听(含 NOTIFY_TX 诊断)

// 事件下行队列:notify_event 只做非阻塞入队,event_worker 串行发送。
// 动机:notify_one 单片最长 ~150ms(mutex 100ms + NOTIFY_TX 50ms),若在产生
// 者上下文同步发送,渲染循环/串口命令/配网会被压住(第 5 轮卡顿项 #6)。
// 专用任务把抖动隔离到事件通道自身。队列 4×512B 静态(2KB,零堆),满则
// 丢帧计数——status 帧带丢帧计数,链路对账可见,不掩盖。
// worker 优先级 3:低于 app_task(4),UI 不被事件发送抢占;与 sound_worker
// 同级(两者无共享资源)。栈 4096:notify 路径含 NimBLE 内部
// (ble_gatts_notify_custom → ble_gattc_log_notify 为 BLE_HS_LOG(INFO),
// 每次 notify 走 ESP_LOGI → vfprintf + newlib 锁链,~2KB)。真机 2048
// 实测 Stack protection fault(backtrace 终止于 vfprintf 锁链)。
#define EVENT_Q_DEPTH 4
static StaticQueue_t s_event_q_static;
static uint8_t s_event_q_storage[EVENT_Q_DEPTH][EVENT_LINE_MAX];  // 项含尾 NUL
static QueueHandle_t s_event_q;
// event_worker 静态栈(.bss):nimble_port_init(controller)吃掉 ~44.7KB heap 后,
// 剩余 largest 仅 ~7KB,动态栈分配与 host 任务(4096B,ESP-IDF 内部动态)争抢,
// 实测导致 host 任务创建失败 → 不广播、连不上。静态化后 heap 全让给 host。
static StackType_t s_event_worker_stack[4096 / sizeof(StackType_t)];   // 4096:EVENT 行发送链实测够(notify_one→gatts 旧固件跑通);5120 使 heap 少了 1KB → ble_hs_start 分配失败重启循环
static StaticTask_t s_event_worker_tcb;

static void event_worker_task(void *param);   // 定义在下方(register 中使用须前置声明)

// ---- 单片 notify:mbuf 池为背压,池空则退避重试,超预算才丢该片 ----
// 返回 false = 这一片没交给 host(调用方按"丢片"处理,音频路径只丢片不丢流)。
static bool notify_one(uint16_t conn_handle, uint16_t val_handle,
                       const void *data, size_t len) {
    // 互斥等待超时 = 另一发送者长时间占用(链路拥塞),按丢片处理
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(TX_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "TX 互斥等待超时(handle=%u),丢片", val_handle);
        return false;
    }
    bool ok = false;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(TX_CHUNK_BUDGET_MS);
    for (;;) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
        if (!om) {
            // 池空:host 还没把在途片交给 controller。等一个连接事件量级再试。
            if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
                ESP_LOGW(TAG, "mbuf 耗尽,超 %d ms 预算丢片(handle=%u len=%u)",
                         TX_CHUNK_BUDGET_MS, val_handle, (unsigned)len);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(TX_RETRY_DELAY_MS));
            continue;
        }
        // 无论成败,notify_custom 都会消费 om(失败路径内部 os_mbuf_free_chain),
        // 因此重试必须重新分配,不能复用上一轮的 om。
        int rc = ble_gatts_notify_custom(conn_handle, val_handle, om);
        if (rc == 0) { ok = true; break; }
        if (rc == BLE_HS_ENOMEM && (int32_t)(xTaskGetTickCount() - deadline) < 0) {
            vTaskDelay(pdMS_TO_TICKS(TX_RETRY_DELAY_MS));
            continue;
        }
        ESP_LOGW(TAG, "notify 失败 rc=%d(handle=%u len=%u)", rc, val_handle, (unsigned)len);
        break;
    }
    xSemaphoreGive(s_tx_mutex);
    return ok;
}

// ---- GATT server 事件:NimBLE 完成一次 notify 传输后回调(host 任务上下文) ----
// IDF 5.5: gatts 事件回调移除,NOTIFY_TX 归入 GAP 事件(见 gap_event_handler)。

// ---- CTRL 写回调:长度校验 + 交还上层解析,零阻塞(不调任何慢路径) ----
static int ctrl_write(uint16_t conn_handle, struct ble_gatt_access_ctxt *ctxt) {
    (void)conn_handle;
    const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len > CTRL_PAYLOAD_MAX) {
        ESP_LOGW(TAG, "CTRL 载荷长度非法 %u(上限 %d)", len, CTRL_PAYLOAD_MAX);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;   // 0x0D
    }
    if (os_mbuf_copydata(ctxt->om, 0, len, s_ctrl_buf) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    s_ctrl_buf[len] = '\0';
    if (s_ctrl_cb) {
        // 解析(cJSON,含深度防护)与路由在回调内完成(passport_voice),
        // 本层不认识协议语义——畸形行由回调记日志并丢弃。
        s_ctrl_cb((const uint8_t *)s_ctrl_buf, len, s_ctrl_user);
        return 0;
    }
    return 0;   // 无消费方:静默接受,避免对端重试风暴
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) return ctrl_write(conn_handle, ctxt);
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_defs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,  // NimBLE 5.5: 宏 BLE_GATT_SVC_DEF 移除
        .uuid = &s_audio_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(CTRL_CHR_UUID),
                .access_cb = gatt_access,
                // WRITE_NO_RSP: Mac 端下行(response=False)免等 ATT 确认 RTT(~5-20ms),
                // 转写预览/审批更快落屏。NimBLE 对 WRITE_REQ 与 WRITE_CMD 都调用同一
                // access_cb(op 同为 BLE_GATT_ACCESS_OP_WRITE_CHR) → 回调零改动。
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP
                       | BLE_GATT_CHR_F_WRITE_ENC,
                .val_handle = &s_ctrl_val_handle,
            },
            {
                .uuid = BLE_UUID16_DECLARE(EVENT_CHR_UUID),
                .access_cb = gatt_access,
                // NOTIFY_INDICATE_ENC: 订阅(写 CCCD)要求已加密连接。NimBLE
                // 对 NOTIFY 特征自动注册 CCCD,该 flag 给自动 CCCD 加
                // WRITE_ENC → 未配对连接订阅被拒(0x0F),macOS 中央访问加密
                // 属性时自动发起配对(Just Works SC),配对后通知自动加密。
                // 防止未配对连接收到明文音频/事件流。
                // (旧写法:手写 0x2902 描述符 —— 5.5 的 count_cfg 校验
                // access_cb 非空且与自动 CCCD 重复,真机 EINVAL 不广播)
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_event_val_handle,
            },
            {
                .uuid = BLE_UUID16_DECLARE(AUDIO_CHR_UUID),
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_audio_val_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

// ---- GAP 事件 ----
static int gap_event_handler(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_NOTIFY_TX:      // NimBLE 5.5: NOTIFY_TX 归入 GAP 事件
        // 仅诊断:该事件在 notify_custom 返回前就已派发("已尝试"),不是完成信号,
        // 不能当流控(见 notify_one 注释)。status != 0 记一条,别刷屏。
        s_tx_status = event->notify_tx.status;
        if (event->notify_tx.status != 0 && event->notify_tx.status != BLE_HS_EDONE) {
            ESP_LOGD(TAG, "NOTIFY_TX status=%d(attr=%u)",
                     event->notify_tx.status, event->notify_tx.attr_handle);
        }
        break;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            ESP_LOGI(TAG, "已连接 (handle %u)", s_conn);
            // 吞吐诊断:连接建立瞬间打印实际参数(1.25ms 单位转毫秒),
            // 与请求值对照可判断 central 是否接受参数更新。
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(s_conn, &desc) == 0) {
                ESP_LOGI(TAG, "连接参数: itvl=%u(%ums) latency=%u timeout=%u",
                         desc.conn_itvl, (unsigned)(desc.conn_itvl * 5 / 4),
                         desc.conn_latency, desc.supervision_timeout);
            }
            // 吞吐工程:2M PHY + 快连接间隔(15-30ms / latency 0);尽力而为,失败不致命
            int rc = ble_gap_set_prefered_le_phy(s_conn, BLE_GAP_LE_PHY_2M_MASK,
                                                 BLE_GAP_LE_PHY_2M_MASK, 0);
            if (rc != 0) ESP_LOGW(TAG, "2M PHY 请求失败 %d", rc);
            const struct ble_gap_upd_params upd = {
                .itvl_min = 12,          // 15ms(1.25ms 单位)
                .itvl_max = 24,          // 30ms
                .latency = 0,
                .supervision_timeout = 400,   // 4s(10ms 单位)
                .min_ce_len = 0,
                .max_ce_len = 0,
            };
            rc = ble_gap_update_params(s_conn, &upd);
            if (rc != 0) ESP_LOGW(TAG, "连接参数更新失败 %d", rc);
            // 吞吐修复:ATT MTU 23 → 185。Mac central 从不主动发起 MTU 交换,
            // 必须外设侧发起;协商后单次 notify 载荷 20B → ~182B,一个 804B
            // ADPCM 帧 = 5 片。真瓶颈是 mbuf 池 + 首片失败即整帧作废(见任务
            // design.md §3),MTU 只是把片数从 41 降到 5,不是流控。
            rc = ble_gattc_exchange_mtu(s_conn, NULL, NULL);
            if (rc != 0) ESP_LOGW(TAG, "MTU 交换失败 %d", rc);
        } else {
            ESP_LOGE(TAG, "桌面端连接失败 (rc=%u)", event->connect.status);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGE(TAG, "语音连接断开 (reason %d)", event->disconnect.reason);
        if (event->disconnect.conn.conn_handle == s_conn) {  // NimBLE 5.5: 句柄移入 conn 描述
            s_conn = 0xFFFF;
            s_audio_conn = 0xFFFF;
            s_event_subscribed = false;
            if (s_state_cb) s_state_cb(false, s_state_user);   // 上层收束会话回 READY
        }
        // 重开广播由 passport_link_ble.c 的断连回调负责
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
        // 吞吐诊断:参数更新请求的最终结果(成功/被拒)
        ESP_LOGI(TAG, "连接参数更新结果: status=%d", event->conn_update.status);
        break;

    case BLE_GAP_EVENT_DATA_LEN_CHG:
        // DLE 协商结果:出现 = 协商成功,LL PDU 上限从此值生效
        ESP_LOGI(TAG, "DLE 协商完成: max_tx=%u rx=%u",
                 event->data_len_chg.max_tx_octets,
                 event->data_len_chg.max_rx_octets);
        break;

    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        // 吞吐诊断:2M PHY 协商结果(1=1M, 2=2M)
        ESP_LOGI(TAG, "PHY 更新结果: status=%d tx_phy=%u rx_phy=%u",
                 event->phy_updated.status, event->phy_updated.tx_phy,
                 event->phy_updated.rx_phy);
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        // EVENT 特征被订阅 = 链路通(PTT 可用);取消订阅 = 链路断
        if (event->subscribe.attr_handle == s_event_val_handle) {
            s_event_subscribed = event->subscribe.cur_notify;
            if (event->subscribe.cur_notify) {
                ESP_LOGI(TAG, "EVENT 特征已订阅,链路通");
                // 吞吐修复(真机实测定位):连接瞬间 NimBLE 自动发的 DLE/
                // 参数更新与 Mac 自身协商撞车(conn update 回 HCI 0x2A
                // Different Transaction Collision,PHY 首轮 35),LL 停在
                // 27B PDU、每连接事件 1 个 → 251B HCI 包 ~98ms,32KB/s
                // 音频实际只过 ~6%。订阅确认 = 链路就绪,此刻重发:
                //  DLE 251B/2120µs:LL PDU 27B→251B
                //  max_ce_len=0xFFFF:每事件多发几个 PDU(2M 下多包/CE)
                // DLE 251B/2120µs:LL PDU 27B→251B。控制器把 set_data_len
                // 当"偏好设置",默认已是 251 → no-op,不会发起 LL_LENGTH_REQ;
                // 先设 27 制造变化,再设 251 强制控制器发起 DLE 协商。
                ble_gap_set_data_len(event->subscribe.conn_handle, 27, 328);
                ble_gap_set_data_len(event->subscribe.conn_handle, 251, 2120);
                struct ble_gap_upd_params upd = {
                    .itvl_min = 9, .itvl_max = 12, .latency = 0,
                    .supervision_timeout = 400,
                    .min_ce_len = 0, .max_ce_len = 0xFFFF,
                };
                ble_gap_update_params(event->subscribe.conn_handle, &upd);
                if (s_state_cb) s_state_cb(true, s_state_user);
                // 链路通是 PTT 的充分条件;回调在 NimBLE host 任务上下文(非 ISR),
                // 实现方只做非阻塞投递。
            } else {
                ESP_LOGW(TAG, "EVENT 订阅取消,链路断");
                if (s_state_cb) s_state_cb(false, s_state_user);
            }
        }
        if (event->subscribe.attr_handle == s_audio_val_handle) {
            if (event->subscribe.cur_notify) {
                s_audio_conn = event->subscribe.conn_handle;
                ESP_LOGI(TAG, "AUDIO 特征已订阅");
            } else if (event->subscribe.conn_handle == s_audio_conn) {
                s_audio_conn = 0xFFFF;
            }
        }
        break;

    default:
        break;
    }
    return 0;
}

// ==================== 对外接口 ====================

void voice_ble_set_ctrl_callback(voice_ble_ctrl_cb_t cb, void *user) {
    s_ctrl_cb = cb;
    s_ctrl_user = user;
}

void voice_ble_set_state_callback(voice_ble_state_cb_t cb, void *user) {
    s_state_cb = cb;
    s_state_user = user;
}

esp_err_t voice_ble_register(void) {
    // 时序契约:由 ble_prov_start() 在 nimble_port_init() 之后、
    // nimble_port_freertos_init() 之前调用(服务表在 host 启动前冻结)。

    // (A) 一次性资源:TX 互斥锁、事件队列、event_worker 任务。这些是 FreeRTOS
    //     对象,不随 NimBLE 的 nimble_port_stop/deinit 销毁,只创建一次。
    static bool s_res_ready;
    if (!s_res_ready) {
        s_tx_mutex = xSemaphoreCreateMutex();
        if (!s_tx_mutex) { ESP_LOGE(TAG, "TX 互斥锁创建失败"); return ESP_ERR_NO_MEM; }
        // 事件下行队列 + worker(静态 2KB,零堆);须在事件产生者(audio worker)
        // 启动前就绪,否则 notify_event 走"队列未建"丢弃路径
        s_event_q = xQueueCreateStatic(EVENT_Q_DEPTH, EVENT_LINE_MAX,
                                       (uint8_t *)s_event_q_storage, &s_event_q_static);
        if (!s_event_q) { ESP_LOGE(TAG, "事件队列创建失败"); return ESP_ERR_NO_MEM; }
        if (!xTaskCreateStatic(event_worker_task, "event_worker", 4096, NULL, 3,
                               s_event_worker_stack, &s_event_worker_tcb)) {
            ESP_LOGE(TAG, "事件 worker 创建失败"); return ESP_ERR_NO_MEM;
        }
        s_res_ready = true;
    }

    // (B) 每次 NimBLE 实例启动都**必须**重做:GATT 服务表 + GAP listener + SMP 配置。
    //     nimble_port_deinit 会拆掉整个 GATT 服务器与 listener 链表。若沿用旧的
    //     "只注册一次"守卫,用户切页/开关蓝牙后再进页 → 语音服务不再进表 → 桌面端
    //     连上后读不到 0xA2B0/0xA2B2(实测 relay "0xA2B2 was not found")。esp_hid 与
    //     配网服务本就每次 ble_prov_start 重注册,语音服务也必须如此。
    //
    // Just Works 无输入输出配对(macOS/Windows 访问加密特征时自动发起);
    // CONFIG_BT_NIMBLE_SM_SC=y 只允许 SC 配对。sync_cb/reset_cb 归 ble_prov.c。
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    // PPT 遥控(HID 键盘)需要 Windows 持久配对才能自动重连:sm_bonding=1 让
    // 配对真正建立 bond(交换并保存 LTK),配合 NVS_PERSIST 持久化到 NVS。
    // 不设 sm_bonding 则即便 NVS_PERSIST 开启,配对也不产生 bond(默认
    // BLE_SM_BONDING=0),Windows 无法自动重连已配对设备。免配对的配网/语音
    // 连接不受影响(对方不要求配对时不走 SMP)。
    ble_hs_cfg.sm_bonding = 1;
    // 配对持久化:CONFIG_BT_NIMBLE_NVS_PERSIST=y 时 sysinit 自动注册 store 回调,
    // 无需手动 ble_store_config_init(5.5 头文件不再导出该函数)。

    int rc = ble_gatts_count_cfg(gatt_defs);
    if (rc != 0) { ESP_LOGE(TAG, "count_cfg 失败 %d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(gatt_defs);
    if (rc != 0) { ESP_LOGE(TAG, "add_svcs 失败 %d", rc); return ESP_FAIL; }
    // 重新挂 GAP listener:先摘再挂,避免上次 deinit 后 s_gap_listener 残留的
    // 链表指针被重复插入(host 内部链表已随 deinit 清空,但结构体是我们的静态量)。
    (void)ble_gap_event_listener_unregister(&s_gap_listener);
    rc = ble_gap_event_listener_register(&s_gap_listener, gap_event_handler, NULL);
    if (rc != 0) { ESP_LOGE(TAG, "listener 注册失败 %d", rc); return ESP_FAIL; }
    // 注意:不显式调用 ble_gatts_start()。NimBLE 5.1+ 的 ble_hs_start() 在 host
    // 启动时内部调用它完成注册(count_cfg/add_svcs 的 defs 在此被消费)。
    // 显式调用 = 二次启动:第一次已注册并释放 defs 列表,host 启动时第二次
    // gatts_start 清空 ATT 条目表且注册循环跑 0 次 → 服务发现全表搜索空 →
    // READ_GRP_TYPE 应答 ERROR 0x0A ATTR_NOT_FOUND → 中央 ~190ms 主动断开
    // (reason 531)。真机 HCI 抓流定位,bleprph 也从不显式调用。

    ESP_LOGI(TAG, "语音 GATT 服务就绪(0xA2B0)");
    return ESP_OK;
}

// 音频帧发送:每片前置 2B 帧头,片级失败只丢该片并继续。
// 唯一调用者是 audio_streamer 的 ble_worker(单任务),故发送暂存可用静态缓冲。
// 帧头是"只丢片不丢帧"的前提:重组端按字节对齐,缺片若无标记会让此后所有块
// 永久错位;有 [块序号][片序号] 后 Mac 只丢受损块,下一块自动重新对齐。
static uint8_t s_audio_tx[256];        // ATT 载荷上限 = MTU-3,MTU ≤ 256(sdkconfig 首选值)
static uint8_t s_audio_blk_seq = 0;    // 块序号,每帧 ++(mod 256)

int voice_ble_notify_audio(const uint8_t *frame, size_t len) {
    if (!frame || len == 0) return -1;
    if (s_conn == 0xFFFF || s_audio_conn == 0xFFFF) {
        drop_count_inc(&s_drop_audio);
        return -1;   // 未连接 / AUDIO CCCD 未订阅:上层走丢帧
    }
    uint16_t mtu = ble_att_mtu(s_conn);
    if (mtu < ATT_MTU_MIN) mtu = ATT_MTU_MIN;   // 协商未完成兜底

    size_t body = (size_t)mtu - PAYLOAD_OVERHEAD - AUDIO_CHUNK_HDR;
    if (body > sizeof(s_audio_tx) - AUDIO_CHUNK_HDR) {
        body = sizeof(s_audio_tx) - AUDIO_CHUNK_HDR;
    }
    if (body == 0) return -1;   // MTU 23 兜底后 body = 18,不会到这;防御性

    const uint8_t seq = s_audio_blk_seq++;
    size_t off = 0;
    unsigned idx = 0, failed = 0;
    while (off < len) {
        size_t n = (len - off) < body ? (len - off) : body;
        bool last = (off + n) >= len;
        s_audio_tx[0] = seq;
        s_audio_tx[1] = (uint8_t)((idx & AUDIO_CHUNK_IDX_MASK) | (last ? AUDIO_CHUNK_LAST : 0));
        memcpy(s_audio_tx + AUDIO_CHUNK_HDR, frame + off, n);
        if (!notify_one(s_audio_conn, s_audio_val_handle, s_audio_tx, n + AUDIO_CHUNK_HDR)) {
            drop_count_inc(&s_drop_audio);
            failed++;   // 只丢该片:继续发后续片,Mac 端解出该块已收到的前缀
        }
        off += n;
        idx++;
    }
    return failed ? -1 : 0;
}

// 事件行实际发送(仅 event_worker 上下文):单包直发或同一分片流控。
// 订阅状态在入队后可能变化,发送前复查;失败丢帧计数(语义同旧同步路径)。
static int send_event_line_now(const char *line, size_t len) {
    if (s_conn == 0xFFFF || !s_event_subscribed) {
        drop_count_inc(&s_drop_event);
        return -1;   // 订阅者缺失:上层走丢帧
    }
    uint16_t mtu = ble_att_mtu(s_conn);
    if (mtu < ATT_MTU_MIN) mtu = ATT_MTU_MIN;

    if (len <= (size_t)mtu - PAYLOAD_OVERHEAD) {
        // 单包直发
        if (!notify_one(s_conn, s_event_val_handle, line, len)) {
            drop_count_inc(&s_drop_event);
            return -1;
        }
        return 0;
    }
    // 跨包:同一分片流控
    voice_ble_packer_t p;
    voice_ble_err_t e = voice_ble_pack_init(&p, (const uint8_t *)line, len, mtu);
    if (e != VOICE_BLE_OK) return -1;
    const uint8_t *chunk;
    size_t clen;
    while ((e = voice_ble_pack_next(&p, &chunk, &clen)) == VOICE_BLE_OK) {
        if (!notify_one(s_conn, s_event_val_handle, chunk, clen)) {
            drop_count_inc(&s_drop_event);
            return -1;
        }
    }
    return 0;
}

// 事件发送专用任务:从队列取行串行发送。FIFO 保证 voice.end/status 等
// 事件间顺序(帧序保证依赖:voice.end 在 drain 后入队,status 紧跟其后)。
static void event_worker_task(void *param) {
    (void)param;
    char item[EVENT_LINE_MAX];
    for (;;) {
        if (xQueueReceive(s_event_q, item, portMAX_DELAY) != pdTRUE) continue;
        // 入队时已写尾 NUL(见 voice_ble_notify_event),按 C 串取长
        send_event_line_now(item, strlen(item));
    }
}

// 事件入队公共实现:timeout_ms=0 即非阻塞(普通路径);>0 为会话边界帧阻塞等待。
static int notify_event_impl(const char *line, size_t len, uint32_t timeout_ms) {
    if (!line || len == 0 || len > EVENT_LINE_MAX) return -1;
    if (s_event_q == NULL || s_conn == 0xFFFF || !s_event_subscribed) {
        drop_count_inc(&s_drop_event);
        return -1;   // 队列未建/订阅者缺失:计数丢弃
    }
    char item[EVENT_LINE_MAX];
    if (len >= sizeof(item)) len = sizeof(item) - 1;   // 协议实际行 ≤511,此处仅防越界
    memcpy(item, line, len);
    item[len] = '\0';
    // 队列满 = 事件通道拥塞(worker 单片 ~150ms 上限):普通路径丢帧计数,
    // status 帧带丢帧计数,链路对账可见;阻塞路径等到 ≤timeout_ms(见 .h)。
    if (xQueueSend(s_event_q, item, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        drop_count_inc(&s_drop_event);
        return -1;
    }
    return 0;
}

int voice_ble_notify_event(const char *line, size_t len) {
    return notify_event_impl(line, len, 0);
}

int voice_ble_notify_event_blocking(const char *line, size_t len, uint32_t timeout_ms) {
    return notify_event_impl(line, len, timeout_ms);
}

bool voice_ble_connected(void) { return s_conn != 0xFFFF; }
bool voice_ble_ready(void) { return s_conn != 0xFFFF && s_event_subscribed; }

uint16_t voice_ble_mtu(void) {
    if (s_conn == 0xFFFF) return 0;
    return ble_att_mtu(s_conn);
}

uint32_t voice_ble_audio_drops(void) {
    portENTER_CRITICAL(&s_drop_mux);
    uint32_t d = s_drop_audio;
    portEXIT_CRITICAL(&s_drop_mux);
    return d;
}
uint32_t voice_ble_event_drops(void) {
    portENTER_CRITICAL(&s_drop_mux);
    uint32_t d = s_drop_event;
    portEXIT_CRITICAL(&s_drop_mux);
    return d;
}

#endif   // ESP_PLATFORM
