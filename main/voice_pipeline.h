// main/voice_pipeline.h —— 语音对话流水线:录音 → ASR → LLM → TTS 播放。
// 全部阻塞操作(音频收发/HTTP)都在本模块的 worker 任务里,按键回调只投递命令。
// chat_core_send() 仅由本 worker 调用(单写者),页面经回调收到状态变化后再刷 UI。
#pragma once

#include "chat_core.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// worker 上下文回调(运行于 worker 任务;UI 实现内部必须持 bsp_lvgl_lock 并容忍页面已删)。
typedef struct {
    void (*on_state)(chat_state_t st, const char *error, void *user);
    void (*on_message)(const char *role, const char *text, void *user);   // 已入历史的新消息
    void *user;
} pipeline_callbacks_t;

// 创建 worker 任务与命令队列(页面 enter 时调用一次)。
esp_err_t voice_pipeline_start(const pipeline_callbacks_t *cbs);

// 握手停机:通知 worker 退出并等待其结束当前操作(最多约 25s),页面 exit 时调用。
// 返回前保证不再有任何回调/音频操作(正常路径)。
void voice_pipeline_stop(void);

// DOWN 按下:开始一轮 录音→识别→回答→播放(空闲/错误态才生效)。
void voice_pipeline_start_record(void);

// 用户中断:中止录音/跳过网络等待的后续动作/停止播放。
void voice_pipeline_abort(void);

bool voice_pipeline_busy(void);

#ifdef __cplusplus
}
#endif
