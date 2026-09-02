// components/passport_voice/include/passport_voice.h
// 语音输入功能(PTT 按住说话)统一入口。
//
// 移植自 folo-ai-passport-voice:按住说话 → 16kHz 音频 100ms 帧经 BLE(0xA2B0
// 服务)流式上行 → 桌面端 companion 转发火山流式 ASR → 识别文本回显本机屏幕。
//
// 分层约定(与仓库架构一致):
//   passport_link      拥有 NimBLE 生命周期与 0xA2B0 GATT 传输(passport_link_voice)
//   passport_voice     本组件:音频管线/ADPCM/协议编解码/提示音(全部 worker 化)
//   main.c             语音视图状态机与按键映射(LVGL 只在 system_task 触碰)
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 转写结果下行(桌面端 ASR)。partial=false 为中间预览,true 为定稿。
// 回调在 NimBLE host task 上下文,实现方只做非阻塞投递(如 xQueueSend)。
typedef void (*passport_voice_transcript_cb_t)(const char *text, bool final, void *user);

// BLE 链路通断(EVENT 特征订阅 = 通;断连/取消订阅 = 断)。回调上下文同上。
typedef void (*passport_voice_link_cb_t)(bool up, void *user);

// 音频硬件错误(采集自停)。回调在 audio worker 任务上下文,只做非阻塞投递。
typedef void (*passport_voice_error_cb_t)(void *user);

/**
 * 初始化语音功能:确保 codec 就绪(16k/16/1,与按键音共用守卫)、建立音频
 * 管线与提示音 worker、绑定 BLE 发送函数、注册 CTRL 下行解析。幂等。
 * 须在 passport_settings_init() 之后调用;不依赖 passport_link_init 时序。
 */
esp_err_t passport_voice_init(void);

void passport_voice_set_transcript_callback(passport_voice_transcript_cb_t cb, void *user);
void passport_voice_set_link_callback(passport_voice_link_cb_t cb, void *user);
void passport_voice_set_error_callback(passport_voice_error_cb_t cb, void *user);

/** 链路就绪(已连接且桌面端已订阅 EVENT 通道)= PTT 可用的充分条件。 */
bool passport_voice_link_ready(void);

/** 反馈音(异步)。MVP 只暴露错误音:链路未就绪按键/会话开启失败。 */
typedef enum { PASSPORT_VOICE_TONE_ERROR = 0 } passport_voice_tone_t;
void passport_voice_play_tone(passport_voice_tone_t tone);

/**
 * 开启一次录音会话:发送 voice.start(会话边界帧,阻塞入队 ≤200ms)→ 播放
 * 开始提示音(异步,与采集并发,按下即录)→ 启动音频采集管线。
 * 失败(管线未就绪)返回非 ESP_OK,调用方不得进入录音态。
 */
esp_err_t passport_voice_start_session(void);

/**
 * 正常结束会话:停采集 → 等残留帧排空(≤500ms)→ 取走丢帧计数 → 发送
 * voice.end 与 status 对账帧 → 播放发送提示音。之后进入等待转写状态。
 * drop_count 输出本会话丢帧数(可为 NULL)。
 */
esp_err_t passport_voice_stop_session(uint32_t *drop_count);

/**
 * 取消会话(误触/断链/退出转写):作废会话 token,环内残留与迟到帧静默丢弃,
 * 不发送任何边界帧。快速返回,无需排空。
 */
void passport_voice_cancel_session(void);

/** 当前录音峰值(0-32767), 用于 UI 音量条。非录音态返回 0。 */
uint16_t passport_voice_peak(void);

#ifdef __cplusplus
}
#endif
