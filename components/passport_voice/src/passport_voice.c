// components/passport_voice/src/passport_voice.c
// 语音功能适配层:把移植的纯功能模块(音频管线/协议/提示音)接到
// passport_link 的 0xA2B0 传输与 main.c 的回调之上。
#include "passport_voice.h"

#include "voice_ble.h"
#include "passport_settings.h"
#include "audio_streamer.h"
#include "voice_protocol.h"
#include "voice_sound.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "passport_voice";

static bool s_inited;
static passport_voice_transcript_cb_t s_transcript_cb;
static void *s_transcript_user;
static passport_voice_link_cb_t s_link_cb;
static void *s_link_user;
static passport_voice_error_cb_t s_error_cb;
static void *s_error_user;

// CTRL 下行(NimBLE host task 上下文):解析 JSON 行,只路由本功能关心的事件。
// agent.* / approval / time.set 等下行当前无消费方,静默忽略(协议向前兼容)。
static void on_ctrl_line(const uint8_t *data, size_t len, void *user) {
    (void)user;
    app_event_t ev;
    if (!app_protocol_parse((const char *)data, len, &ev)) {
        ESP_LOGW(TAG, "CTRL 行拒绝: %.*s", (int)(len > 80 ? 80 : len), (const char *)data);
        return;
    }
    if (ev.type == APP_EV_TRANSCRIPT && s_transcript_cb) {
        s_transcript_cb(ev.u.transcript.text, ev.u.transcript.final, s_transcript_user);
    }
}

static void on_link_state(bool up, void *user) {
    (void)user;
    if (s_link_cb) s_link_cb(up, s_link_user);
}

static void on_streamer_event(audio_streamer_evt_t evt, void *user) {
    (void)evt;
    (void)user;
    if (s_error_cb) s_error_cb(s_error_user);
}

esp_err_t passport_voice_init(void) {
    if (s_inited) return ESP_OK;
    // codec 懒初始化守卫与按键音共用(16k/16/1 = 录音同格式,全程不换格式)
    if (!passport_settings_ensure_audio_ready()) {
        ESP_LOGW(TAG, "音频 codec 不可用,语音功能降级");
        return ESP_FAIL;
    }
    audio_streamer_set_sender(voice_ble_notify_audio);
    audio_streamer_set_compressed(true);
    audio_streamer_set_event_cb(on_streamer_event, NULL);
    esp_err_t err = audio_streamer_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "音频管线初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    err = voice_sound_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "提示音初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    voice_ble_set_ctrl_callback(on_ctrl_line, NULL);
    voice_ble_set_state_callback(on_link_state, NULL);
    s_inited = true;
    ESP_LOGI(TAG, "语音功能就绪");
    return ESP_OK;
}

void passport_voice_set_transcript_callback(passport_voice_transcript_cb_t cb, void *user) {
    s_transcript_cb = cb;
    s_transcript_user = user;
}

void passport_voice_set_link_callback(passport_voice_link_cb_t cb, void *user) {
    s_link_cb = cb;
    s_link_user = user;
}

void passport_voice_set_error_callback(passport_voice_error_cb_t cb, void *user) {
    s_error_cb = cb;
    s_error_user = user;
}

bool passport_voice_link_ready(void) {
    return voice_ble_ready();
}

void passport_voice_play_tone(passport_voice_tone_t tone) {
    voice_sound_play(APP_TONE_ERROR);   // MVP 仅错误音;音色表在 voice_sound 内
    (void)tone;
}

esp_err_t passport_voice_start_session(void) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    // 先开采集(失败即不发任何边界帧,桌面端无悬挂会话),再发 voice.start,
    // 最后提示音(异步并发,按下即录语义)。
    esp_err_t err = audio_streamer_start();
    if (err != ESP_OK) return err;
    char buf[APP_PROTO_TX_CAP];
    size_t len = app_protocol_voice_start(buf, sizeof(buf), "ima_adpcm");
    if (len) voice_ble_notify_event_blocking(buf, len, 200);
    voice_sound_play(APP_TONE_START);
    return ESP_OK;
}

esp_err_t passport_voice_stop_session(uint32_t *drop_count) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    // 帧序:停采集 → drain(残留帧先出环)→ 取丢帧计数 → voice.end → status。
    // 停采集顺带等 audio_worker 退出阻塞读,SEND 提示音不会叠进采集尾。
    audio_streamer_stop();
    audio_streamer_drain(500);
    uint32_t drops = audio_streamer_take_drops();
    if (drop_count) *drop_count = drops;
    char buf[APP_PROTO_TX_CAP];
    size_t len = app_protocol_voice_end(buf, sizeof(buf));
    if (len) voice_ble_notify_event_blocking(buf, len, 200);
    len = app_protocol_device_status(buf, sizeof(buf), drops);
    if (len) voice_ble_notify_event(buf, len);
    voice_sound_play(APP_TONE_SEND);
    return ESP_OK;
}

void passport_voice_cancel_session(void) {
    audio_streamer_cancel();
}

uint16_t passport_voice_peak(void) {
    return audio_streamer_peak();
}
