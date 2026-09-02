// voice_sound.h —— 提示音模块。
// 方波提示音经 bsp_audio_write 输出(移植自 demo_audio.c 的 play_tone 模式)。
// 16kHz/16bit/单声道 —— 与录音流同格式;codec 初始化由
// passport_settings_ensure_audio_ready() 统一负责,本模块不碰 codec。
#pragma once

#include "voice_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 建静态队列 + sound_worker 任务(prio 5, 栈 2KB)。须在音频 codec 就绪后调用
// (见 passport_settings_ensure_audio_ready)。
esp_err_t voice_sound_init(void);

// 异步播放:入队由 sound_worker 播出,返回是否入队成功。
// START 音与采集并发(全双工 I2S,按下即录语义),无需等待播完。
bool voice_sound_play(app_tone_t tone);

// 同步播放:在调用者上下文中阻塞播完。仅保留给非会话场景的确定性播放。
void voice_sound_play_sync(app_tone_t tone);

#ifdef __cplusplus
}
#endif
