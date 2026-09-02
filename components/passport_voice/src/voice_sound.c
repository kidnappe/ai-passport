// voice_sound.c —— 提示音实现(移植自 folo-ai-passport-voice main/app_sound.c)。
// 每个音由一个或多个方波段组成(频率×时长),段间无缝衔接。
// 全部走 16kHz/16bit/单声道 —— 与录音流同格式,bsp_audio_set_format 只调一次。
#include "voice_sound.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "voice_sound";

#define SAMPLE_RATE 16000
#define CHUNK_SAMPLES 256   // 每块 ~16ms,控制栈上临时缓冲
#define AMPLITUDE 5000      // 方波幅度(±),低于 demo 的 6000 稍柔和

// ---- 音色表:各音 = 若干 {频率Hz, 时长ms} 段 ----
typedef struct { uint16_t hz; uint16_t ms; } tone_seg_t;

static const tone_seg_t TONE_TABLE[APP_TONE_COUNT][3] = {
    [APP_TONE_START]    = { { 440, 80 },  { 0, 0 }, { 0, 0 } },  // 录音就绪
    [APP_TONE_SEND]     = { { 880, 60 },  { 0, 0 }, { 0, 0 } },  // 语音已发送
    [APP_TONE_APPROVAL] = { { 587, 75 },  { 880, 75 }, { 0, 0 } }, // 审批提醒(双音)
    [APP_TONE_SUCCESS]  = { { 660, 100 }, { 880, 100 }, { 0, 0 } }, // 完成(上行音)
    [APP_TONE_REJECT]   = { { 220, 120 }, { 0, 0 }, { 0, 0 } },  // 拒绝
    [APP_TONE_ERROR]    = { { 180, 100 }, { 0, 0 }, { 0, 0 } },  // 离线/非法
};

// 播放一个段:方波整块生成 + 阻塞写 I2S。
static void play_segment(uint16_t hz, uint16_t ms) {
    int16_t buf[CHUNK_SAMPLES];
    const int period = SAMPLE_RATE / hz;         // 每周期采样数
    const int half = period / 2;
    int total = (int)SAMPLE_RATE * ms / 1000;
    int phase = 0;
    while (total > 0) {
        int n = total < CHUNK_SAMPLES ? total : CHUNK_SAMPLES;
        for (int i = 0; i < n; i++) {
            buf[i] = (phase < half) ? AMPLITUDE : -AMPLITUDE;
            if (++phase >= period) phase = 0;
        }
        if (bsp_audio_write(buf, (size_t)n * sizeof(int16_t)) != ESP_OK) {
            ESP_LOGE(TAG, "bsp_audio_write 失败");
            return;
        }
        total -= n;
    }
}

static void play_tone_impl(app_tone_t tone) {
    if (tone >= APP_TONE_COUNT) return;
    const tone_seg_t (*segs)[3] = &TONE_TABLE[tone];
    for (int i = 0; i < 3; i++) {
        if ((*segs)[i].hz == 0) break;
        play_segment((*segs)[i].hz, (*segs)[i].ms);
    }
}

// ---- 异步播放:静态队列 + worker ----
static StaticQueue_t s_queue_struct;
static uint8_t s_queue_storage[APP_TONE_COUNT * sizeof(uint8_t)];
static QueueHandle_t s_queue;

static void sound_worker(void *arg) {
    (void)arg;
    uint8_t tone;
    for (;;) {
        if (xQueueReceive(s_queue, &tone, portMAX_DELAY) == pdTRUE) {
            play_tone_impl((app_tone_t)tone);
        }
    }
}

esp_err_t voice_sound_init(void) {
    // codec 初始化与 16k/16/1 格式由 passport_settings_ensure_audio_ready()
    // 统一负责(按键音/音量预览/语音共用一份懒初始化守卫),此处只建队列与 worker。

    s_queue = xQueueCreateStatic(APP_TONE_COUNT, sizeof(uint8_t),
                                 s_queue_storage, &s_queue_struct);
    if (!s_queue) return ESP_FAIL;
    // 静态栈(.bss):1536B 够提示音(单段 256 采样 buf);heap 极限下每字节
    // 都决定 host 任务(5120B 动态创建)能否成功。
    // 1536B 时高水位仅剩 316B(2026-08-28 实测)→ 2048B。
    static StackType_t s_snd_stack[2048 / sizeof(StackType_t)];
    static StaticTask_t s_snd_tcb;
    // 优先级 5(2026-08-29 由 3 提升,与 ble_worker 同级,仍低于 audio_worker 6):
    // 开录音的滴声播完才开流(TONE_DONE 门禁),prio 3 时它排在 app_task(4)与
    // taskLVGL(4,2 行缓冲刷 240x320)后面 —— 真机实测长按判定到 `采集开始` 要
    // 578ms,而滴声本身只有 80ms。提级后这段回落到 ~80ms。提示音只有几十毫秒的
    // 阻塞 I2S 写(等 DMA 时让出 CPU),不会饿死同级/低级任务;且它与采集不重叠。
    if (!xTaskCreateStatic(sound_worker, "sound_worker", 2048, NULL, 5,
                           s_snd_stack, &s_snd_tcb)) {
        ESP_LOGE(TAG, "sound worker 创建失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "提示音就绪");
    return ESP_OK;
}

bool voice_sound_play(app_tone_t tone) {
    if (xQueueSend(s_queue, &tone, 0) != pdTRUE) {
        ESP_LOGW(TAG, "提示音队列满,丢弃 %d", (int)tone);
        return false;
    }
    return true;
}

void voice_sound_play_sync(app_tone_t tone) {
    play_tone_impl(tone);
}
