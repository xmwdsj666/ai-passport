// main/chat_core.h —— AI 语音对话的纯逻辑核心:状态机、会话历史、请求组装、响应解析。
// 本模块禁止依赖 ESP-IDF / LVGL / FreeRTOS,保持主机可测(validate.sh --static 编译运行)。
//
// 线程约定:chat_core_send() 只允许 voice_pipeline worker 任务调用(单一写者);
// 页面按键通过 pipeline 队列间接触发事件;UI 读状态经由 worker 的状态回调。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- 状态机 ----

typedef enum {
    CHAT_IDLE = 0,        // 空闲,等待按住说话
    CHAT_RECORDING,       // 录音中(按住 DOWN)
    CHAT_RECOGNIZING,     // ASR 上传识别中
    CHAT_THINKING,        // LLM 生成中
    CHAT_SPEAKING,        // TTS 播放中
    CHAT_ERROR,           // 出错(提示后任意键回 IDLE)
} chat_state_t;

typedef enum {
    CHAT_EV_START_REC = 0,   // 按住说话键
    CHAT_EV_STOP_REC,        // 松开(录音数据有效)
    CHAT_EV_REC_TOO_SHORT,   // 松开但时长不足,丢弃
    CHAT_EV_REC_FAIL,        // 录音失败(缓冲分配失败/音频设备错误)
    CHAT_EV_ASR_OK,          // 识别成功,文本已入历史
    CHAT_EV_ASR_FAIL,        // 识别失败(网络/空文本)
    CHAT_EV_LLM_OK,          // 回答生成完毕,文本已入历史
    CHAT_EV_LLM_FAIL,        // 生成失败
    CHAT_EV_TTS_OK,          // 播放完毕
    CHAT_EV_TTS_FAIL,        // 播放失败(回答文本已显示)
    CHAT_EV_INTERRUPT,       // 用户中断(OK 短按)
    CHAT_EV_RESET,           // 错误态恢复/清理
} chat_event_t;

// 复位状态与历史(仅初始化/页面进入时调用)。
void chat_core_init(void);

chat_state_t chat_core_state(void);
const char  *chat_core_state_name(chat_state_t st);   // 稳定英文短名,用于日志/状态栏兜底
const char  *chat_core_error(void);                   // 最近错误描述;无错误返回 ""
void         chat_core_send(chat_event_t ev);         // 非法迁移忽略(安全),不崩溃
bool         chat_core_active(void);                  // 非 IDLE 即活跃(退出页面时判忙)

// ---- 会话历史(环形,固定上限;文本指针由内部静态缓冲持有) ----

#define CHAT_HISTORY_MAX 8        // 最多保留 8 条(4 轮问答)
#define CHAT_TEXT_MAX    512      // 单条文本上限(字节,含结尾 NUL)

void         chat_history_add(const char *role, const char *text); // 截断到上限,超容量丢最旧
int          chat_history_count(void);
const char  *chat_history_role(int idx);   // 越界返回 NULL
const char  *chat_history_text(int idx);
void         chat_history_reset(void);

// 把 text 编码为 JSON 字符串字面量(含两端引号)写入 out,返回写入字节数;容量不足返回 0。
// 供需要手工拼请求体的模块(如 TTS)复用,与 build_json 的转义规则一致。
size_t chat_json_escape(char *out, size_t cap, const char *text);

// 组装 /chat/completions 请求体。成功返回 buf,容量不足返回 NULL。
// 现有历史全部带上;最后一条应为刚识别的用户文本(调用方先 chat_history_add)。
char *chat_history_build_json(char *buf, size_t cap,
                              const char *model, const char *system_prompt);

// ---- 解析(无副作用;转义 \n \t \r \" \\ \/ \b \f 与 \uXXXX 含代理对) ----

// 从 /chat/completions 响应取 choices[0].message.content。找到返回 out,否则 NULL。
char *chat_llm_parse_choice(const char *json, char *out, size_t cap);

// 从 /audio/transcriptions 响应取 {"text": "..."}。找到返回 out,否则 NULL。
char *chat_asr_parse_text(const char *json, char *out, size_t cap);

// ---- WAV(上传封装 + TTS 响应探测,均为纯内存操作) ----

// 写标准 44 字节 PCM WAV 头。out 至少 44 字节;data_bytes 为其后 PCM 字节数。
size_t chat_wav_header(uint8_t out[44], uint32_t sample_rate,
                       uint16_t channels, uint16_t bits, uint32_t data_bytes);

// 探测 WAV 流头部(要求 len>=44 且包含 fmt/data 块)。成功返回 data 坽数据偏移,
// 并带出实际格式;非 WAV/头不完整返回 0(调用方继续缓冲)。
size_t chat_wav_probe(const uint8_t *data, size_t len,
                      uint32_t *sample_rate, uint16_t *channels, uint16_t *bits);

// ---- ASR multipart/form-data ----

#define CHAT_MULTIPART_BOUNDARY "----ai-passport-chat-boundary"

// 构造 model 表单字段 + file part 头(到 CRLF CRLF,其后紧跟 WAV 头与 PCM 数据)。
// 返回字节数,容量不足 0。
size_t chat_multipart_head(char *buf, size_t cap, const char *model,
                           const char *filename, uint32_t sample_rate, uint32_t pcm_bytes);
// 构造结尾 "--boundary--";返回字节数,容量不足 0。
size_t chat_multipart_tail(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
