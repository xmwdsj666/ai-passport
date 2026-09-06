// main/ai_client.h —— OpenAI 兼容服务 HTTP 客户端:ASR / chat / TTS。
// 全部端点、模型、Key 来自 Kconfig(sdkconfig),代码不保存任何凭证。
// 同步阻塞接口,只能由 worker 任务调用(禁止按键回调/LVGL 任务)。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// TTS 音频数据块回调:在 ai_client_tts_stream 的调用任务里执行,可阻塞(写喇叭)。
typedef void (*ai_tts_sink_t)(const uint8_t *data, size_t len, void *user);

// 语音识别:上传 16bit 单声道 PCM(内部封装 WAV 头,multipart/form-data)。
// 成功时把识别文本写入 out(可为空串);网络/HTTP 错误返回错误码。
esp_err_t ai_client_asr(const uint8_t *pcm, size_t pcm_bytes, uint32_t sample_rate,
                        char *out, size_t cap);

// 对话:用当前会话历史(最后一条应为刚识别的用户文本)请求 /chat/completions。
// 成功把回答写入 out 并追加进会话历史;失败返回错误码(历史保持一致,不残留半轮)。
esp_err_t ai_client_chat(char *out, size_t cap);

// 语音合成:请求 /audio/speech(response_format=wav),边收边把音频块交给 sink。
// 成功时带出实际采样参数(供 bsp_audio_set_format);用户中止返回 ESP_OK,
// 调用方用自身 abort 标志区分完成与中止。
esp_err_t ai_client_tts_stream(const char *text,
                               uint32_t *out_sample_rate, uint16_t *out_channels,
                               uint16_t *out_bits,
                               ai_tts_sink_t sink, void *user,
                               bool (*aborted)(void));

#ifdef __cplusplus
}
#endif
