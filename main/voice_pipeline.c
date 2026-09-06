// main/voice_pipeline.c —— 流水线 worker 实现。
// 关键约束(来自硬件指南):bsp_audio_read/write 阻塞且只能在 worker;
// 录音缓冲是板上最大瞬态堆分配,上限可配 + 堆水位保护;
// 退出用"可取消循环 + 显式握手",绝不删除阻塞在 codec I/O 里的任务。
#include "voice_pipeline.h"

#include "ai_client.h"
#include "sdkconfig.h"

#include "bsp_audio.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "voice_pipe";

#define SAMPLE_RATE     16000
#define BYTES_PER_SEC   (SAMPLE_RATE * 2)          // 16bit 单声道
#define MIN_RECORD_BYTES (BYTES_PER_SEC / 2)       // 短于 0.5s 视为误触
#define CHUNK_BYTES     1024                       // 每次读 512 采样
#define BTN_RELEASED_MV 3000                       // 松开约 3300mV(见 bsp_pins.h 窗口)
#define STOP_WAIT_MS    25000                      // 握手最长等待(覆盖 HTTP 20s 超时)

typedef enum { PIPE_CMD_REC = 0, PIPE_CMD_ABORT, PIPE_CMD_QUIT } pipe_cmd_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static pipeline_callbacks_t s_cbs;
static volatile bool s_abort;
static volatile bool s_busy;                  // 正在执行一轮流水线(含 HTTP 等待)
static volatile bool s_quit;                  // 页面已请求停机
static volatile bool s_task_alive;

// TTS 播放参数:ai_client 探测 WAV 头后填入,sink 首次调用时用于 set_format。
static uint32_t s_tts_rate;
static uint16_t s_tts_ch, s_tts_bits;
static bool s_tts_fmt_set;

static void notify_state(void)
{
    if (s_cbs.on_state) s_cbs.on_state(chat_core_state(), chat_core_error(), s_cbs.user);
}

static bool aborted(void)
{
    return s_abort || s_quit;
}

// TTS 音频块落喇叭:首块前按探测到的参数重配 codec(BSP 内部处理 close/open)。
static void tts_sink(const uint8_t *data, size_t len, void *user)
{
    (void)user;
    if (!s_tts_fmt_set) {
        if (s_tts_ch != 1 || s_tts_bits != 16) {
            ESP_LOGE(TAG, "不支持的 TTS 格式 ch=%u bits=%u", s_tts_ch, s_tts_bits);
            return;
        }
        if (bsp_audio_set_format(s_tts_rate, 16, 1) != ESP_OK) {
            ESP_LOGE(TAG, "TTS set_format(%lu) 失败", (unsigned long)s_tts_rate);
            return;
        }
        bsp_audio_set_volume(80);
        s_tts_fmt_set = true;
    }
    bsp_audio_write(data, len);
}

// ---------------- 录音 ----------------

// 返回录音字节数;*out_reason: 0=松开 1=中止 2=超限/堆水位(同松开处理) 3=读失败。
static size_t record_pcm(int16_t *buf, size_t cap_bytes, int *out_reason)
{
    size_t got = 0;
    int64_t start = esp_timer_get_time();
    int64_t max_us = (int64_t)CONFIG_AI_CHAT_REC_MAX_SEC * 1000000;

    while (got + CHUNK_BYTES <= cap_bytes) {
        if (aborted()) { *out_reason = 1; return got; }
        // 按住说话:松开即停(BSP 无释放事件,轮询分压电压判断)
        if (got > 0 && bsp_button_read_mv() > BTN_RELEASED_MV) {
            *out_reason = 0;
            return got;
        }
        if (esp_timer_get_time() - start >= max_us) {
            *out_reason = 2;
            return got;
        }
        if (esp_get_free_heap_size() < CONFIG_AI_CHAT_HEAP_MIN) {
            ESP_LOGW(TAG, "堆水位过低,提前截断录音");
            *out_reason = 2;
            return got;
        }
        if (bsp_audio_read(buf + got / 2, CHUNK_BYTES) != ESP_OK) {
            *out_reason = 3;
            return got;
        }
        got += CHUNK_BYTES;
    }
    *out_reason = 2;                              // 缓冲满,自然截断
    return got;
}

static void handle_record(void)
{
    chat_core_send(CHAT_EV_START_REC);
    notify_state();

    if (bsp_audio_set_format(SAMPLE_RATE, 16, 1) != ESP_OK) {
        chat_core_send(CHAT_EV_REC_FAIL);
        notify_state();
        return;
    }

    size_t cap = (size_t)CONFIG_AI_CHAT_REC_MAX_SEC * BYTES_PER_SEC;
    int16_t *buf = malloc(cap);                   // 上限默认 4s = 128KB,板上最大瞬态分配
    if (!buf) {
        ESP_LOGE(TAG, "录音缓冲 %u 字节分配失败(无 PSRAM,可调小 AI_CHAT_REC_MAX_SEC)",
                 (unsigned)cap);
        chat_core_send(CHAT_EV_REC_FAIL);
        notify_state();
        return;
    }

    int reason = 0;
    size_t bytes = record_pcm(buf, cap, &reason);
    ESP_LOGI(TAG, "录音结束: %u 字节, 原因=%d, 最小堆=%u",
             (unsigned)bytes, reason, (unsigned)esp_get_minimum_free_heap_size());

    if (aborted() && chat_core_state() == CHAT_RECORDING) {
        free(buf);
        chat_core_send(CHAT_EV_INTERRUPT);
        notify_state();
        return;
    }
    if (reason == 3) {
        free(buf);
        chat_core_send(CHAT_EV_REC_FAIL);
        notify_state();
        return;
    }
    if (bytes < MIN_RECORD_BYTES) {
        free(buf);
        chat_core_send(CHAT_EV_REC_TOO_SHORT);
        notify_state();
        return;
    }

    chat_core_send(CHAT_EV_STOP_REC);             // → RECOGNIZING
    notify_state();

    // ---- ASR ----
    char text[CHAT_TEXT_MAX];
    esp_err_t err = ai_client_asr((const uint8_t *)buf, bytes, SAMPLE_RATE,
                                  text, sizeof(text));
    free(buf);                                    // 识别完成即释放,给 TLS/TTS 腾堆
    if (aborted() && chat_core_state() == CHAT_RECOGNIZING) {
        chat_core_send(CHAT_EV_INTERRUPT);
        notify_state();
        return;
    }
    if (err != ESP_OK || text[0] == '\0') {
        if (err == ESP_OK) ESP_LOGW(TAG, "ASR 返回空文本");
        chat_core_send(CHAT_EV_ASR_FAIL);
        notify_state();
        return;
    }
    chat_history_add("user", text);
    if (s_cbs.on_message) s_cbs.on_message("user", text, s_cbs.user);
    chat_core_send(CHAT_EV_ASR_OK);               // → THINKING
    notify_state();

    // ---- LLM(上一条历史即刚识别的用户文本) ----
    char reply[CHAT_TEXT_MAX];
    err = ai_client_chat(reply, sizeof(reply));
    if (aborted() && chat_core_state() == CHAT_THINKING) {
        chat_core_send(CHAT_EV_INTERRUPT);
        notify_state();
        return;
    }
    if (err != ESP_OK || reply[0] == '\0') {
        chat_core_send(CHAT_EV_LLM_FAIL);
        notify_state();
        return;
    }
    if (s_cbs.on_message) s_cbs.on_message("assistant", reply, s_cbs.user);
    chat_core_send(CHAT_EV_LLM_OK);               // → SPEAKING
    notify_state();

    // ---- TTS 流式播放 ----
    s_tts_fmt_set = false;
    err = ai_client_tts_stream(reply, &s_tts_rate, &s_tts_ch, &s_tts_bits,
                               tts_sink, NULL, aborted);
    if (aborted() && chat_core_state() == CHAT_SPEAKING) {
        chat_core_send(CHAT_EV_INTERRUPT);
    } else if (err != ESP_OK) {
        chat_core_send(CHAT_EV_TTS_FAIL);
    } else {
        chat_core_send(CHAT_EV_TTS_OK);
    }
    notify_state();
}

// ---------------- worker 任务 ----------------

static void worker(void *arg)
{
    (void)arg;
    s_task_alive = true;
    pipe_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        if (cmd == PIPE_CMD_QUIT) break;
        if (cmd == PIPE_CMD_ABORT) {
            s_abort = true;                       // 正在阻塞的 HTTP 返回后在各检查点生效
            continue;
        }
        if (cmd == PIPE_CMD_REC && !s_busy) {
            s_busy = true;
            s_abort = false;
            handle_record();
            s_busy = false;
        }
    }
    s_task_alive = false;
    vTaskDelete(NULL);                            // 握手完成后自行退出
}

// ---------------- 对外接口 ----------------

esp_err_t voice_pipeline_start(const pipeline_callbacks_t *cbs)
{
    if (s_task_alive) return ESP_ERR_INVALID_STATE;
    if (!cbs) return ESP_ERR_INVALID_ARG;
    s_cbs = *cbs;
    s_queue = xQueueCreate(4, sizeof(pipe_cmd_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    // 8KB 栈:HTTP+TLS 握手在本任务内执行,mbedTLS 峰值需求较大
    if (xTaskCreate(worker, "voice_pipe", 8192, NULL, 4, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void voice_pipeline_stop(void)
{
    if (!s_queue || !s_task_alive) return;
    s_quit = true;
    pipe_cmd_t cmd = PIPE_CMD_QUIT;
    xQueueSend(s_queue, &cmd, 0);
    // 等待 worker 处理完当前操作并退出(音频循环每个分块检查 s_quit,可及时脱身;
    // 卡在 HTTP 时最多等到响应/超时返回)。
    int waited = 0;
    while (s_task_alive && waited < STOP_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited += 50;
    }
    if (s_task_alive) ESP_LOGE(TAG, "worker 停机握手超时(卡在阻塞操作)");
    if (s_queue) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    s_task = NULL;
}

void voice_pipeline_start_record(void)
{
    if (!s_queue || s_quit) return;
    pipe_cmd_t cmd = PIPE_CMD_REC;
    xQueueSend(s_queue, &cmd, 0);
}

void voice_pipeline_abort(void)
{
    if (!s_queue) return;
    pipe_cmd_t cmd = PIPE_CMD_ABORT;
    xQueueSend(s_queue, &cmd, 0);
}

bool voice_pipeline_busy(void)
{
    return s_busy;
}
