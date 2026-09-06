// main/ai_client.c —— OpenAI 兼容 HTTP 客户端实现。
// 每次请求独立创建/销毁 esp_http_client;TLS 用系统证书 bundle;
// 响应体按需读入有限缓冲,杜绝无界内存增长(无 PSRAM)。
#include "ai_client.h"

#include "chat_core.h"
#include "sdkconfig.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ai_client";

#define HTTP_TIMEOUT_MS   20000
#define RESP_MAX          8192      // JSON 响应上限(截断即判错)
#define IO_CHUNK          2048      // 发送/接收分块
#define TTS_PROBE_WINDOW  8192      // WAV 头探测窗口

// base_url + path 拼接,自动避免双斜杠。
static esp_err_t build_url(char *out, size_t cap, const char *path)
{
    const char *base = CONFIG_AI_CHAT_BASE_URL;
    if (base[0] == '\0') {
        ESP_LOGE(TAG, "未配置 AI_CHAT_BASE_URL(menuconfig)");
        return ESP_ERR_INVALID_STATE;
    }
    size_t n = strlen(base);
    int w = snprintf(out, cap, "%s%s%s", base, (base[n - 1] == '/') ? "" : "/", path);
    if (w < 0 || (size_t)w >= cap) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

static esp_err_t set_auth(esp_http_client_handle_t client)
{
    char auth[256 + 8];
    snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_AI_CHAT_API_KEY);
    return esp_http_client_set_header(client, "Authorization", auth);
}

static esp_http_client_handle_t http_open(const char *url)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = IO_CHUNK,
        .buffer_size_tx = IO_CHUNK,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    return esp_http_client_init(&cfg);
}

// 读取响应到 malloc 缓冲(上限 RESP_MAX)。返回 ESP_OK 时 *out_buf 供调用方 free。
static esp_err_t read_response(esp_http_client_handle_t client,
                               char **out_buf, size_t *out_len, int *status)
{
    *out_buf = NULL;
    *out_len = 0;
    *status = esp_http_client_get_status_code(client);
    if (*status != 200) {
        // 非 200:读一小段错误体帮助定位(鉴权失败/模型名错等),不进解析。
        char errbody[256] = { 0 };
        int total = 0;
        while (total < (int)sizeof(errbody) - 1) {
            int n = esp_http_client_read(client, errbody + total, sizeof(errbody) - 1 - total);
            if (n <= 0) break;
            total += n;
        }
        errbody[total] = '\0';
        ESP_LOGE(TAG, "HTTP %d: %.200s", *status, errbody);
        return (*status == 401) ? ESP_ERR_NOT_SUPPORTED : ESP_FAIL;
    }

    char *buf = malloc(RESP_MAX);
    if (!buf) return ESP_ERR_NO_MEM;
    size_t used = 0;
    while (used < RESP_MAX - 1) {
        int n = esp_http_client_read(client, buf + used, (int)(RESP_MAX - 1 - used));
        if (n <= 0) break;
        used += (size_t)n;
    }
    buf[used] = '\0';
    *out_buf = buf;
    *out_len = used;
    return ESP_OK;
}

// ---------------- ASR ----------------

esp_err_t ai_client_asr(const uint8_t *pcm, size_t pcm_bytes, uint32_t sample_rate,
                        char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if (!pcm || pcm_bytes == 0) return ESP_ERR_INVALID_ARG;

    char url[256];
    esp_err_t err = build_url(url, sizeof(url), "/audio/transcriptions");
    if (err != ESP_OK) return err;

    char head[320];
    size_t head_len = chat_multipart_head(head, sizeof(head), CONFIG_AI_CHAT_ASR_MODEL,
                                          "audio.wav", sample_rate, (uint32_t)pcm_bytes);
    char tail[64];
    size_t tail_len = chat_multipart_tail(tail, sizeof(tail));
    if (head_len == 0 || tail_len == 0) return ESP_ERR_INVALID_SIZE;

    uint8_t wav[44];
    chat_wav_header(wav, sample_rate, 1, 16, (uint32_t)pcm_bytes);

    esp_http_client_handle_t client = http_open(url);
    if (!client) return ESP_ERR_NO_MEM;

    char ctype[96];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", CHAT_MULTIPART_BOUNDARY);
    esp_http_client_set_header(client, "Content-Type", ctype);
    set_auth(client);

    size_t total = head_len + sizeof(wav) + pcm_bytes + tail_len;
    err = ESP_FAIL;
    if (esp_http_client_open(client, (int)total) != ESP_OK) {
        ESP_LOGE(TAG, "ASR 连接失败");
        goto done;
    }
    // 先写完全部请求体,再等响应头(顺序反了会与服务器互等)
    if (esp_http_client_write(client, head, (int)head_len) < 0 ||
        esp_http_client_write(client, (const char *)wav, sizeof(wav)) < 0) {
        ESP_LOGE(TAG, "ASR 上传写失败");
        goto done;
    }
    for (size_t off = 0; off < pcm_bytes; off += IO_CHUNK) {
        size_t n = pcm_bytes - off < IO_CHUNK ? pcm_bytes - off : IO_CHUNK;
        if (esp_http_client_write(client, (const char *)pcm + off, (int)n) < 0) {
            ESP_LOGE(TAG, "ASR 音频写失败@%u", (unsigned)off);
            goto done;
        }
    }
    if (esp_http_client_write(client, tail, (int)tail_len) < 0) goto done;
    esp_http_client_fetch_headers(client);

    {
        char *resp = NULL;
        size_t resp_len = 0;
        int status = 0;
        err = read_response(client, &resp, &resp_len, &status);
        if (err == ESP_OK) {
            if (!chat_asr_parse_text(resp, out, cap)) {
                ESP_LOGE(TAG, "ASR 响应解析失败: %.200s", resp);
                err = ESP_ERR_INVALID_RESPONSE;
            }
        }
        free(resp);
    }
done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

// ---------------- chat/completions ----------------

esp_err_t ai_client_chat(char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';

    char url[256];
    esp_err_t err = build_url(url, sizeof(url), "/chat/completions");
    if (err != ESP_OK) return err;

    char *body = malloc(RESP_MAX);                // 与响应缓冲同上限:8 条历史 x 512B 绰绰有余
    if (!body) return ESP_ERR_NO_MEM;
    if (!chat_history_build_json(body, RESP_MAX,
                                 CONFIG_AI_CHAT_LLM_MODEL, CONFIG_AI_CHAT_SYSTEM_PROMPT)) {
        ESP_LOGE(TAG, "请求体组装失败(历史过大?)");
        free(body);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_handle_t client = http_open(url);
    if (!client) {
        free(body);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    set_auth(client);

    err = ESP_FAIL;
    if (esp_http_client_open(client, (int)strlen(body)) != ESP_OK) {
        ESP_LOGE(TAG, "chat 连接失败");
        goto done;
    }
    if (esp_http_client_write(client, body, (int)strlen(body)) < 0) {
        ESP_LOGE(TAG, "chat 请求写失败");
        goto done;
    }
    esp_http_client_fetch_headers(client);

    {
        char *resp = NULL;
        size_t resp_len = 0;
        int status = 0;
        err = read_response(client, &resp, &resp_len, &status);
        if (err == ESP_OK) {
            if (!chat_llm_parse_choice(resp, out, cap)) {
                ESP_LOGE(TAG, "chat 响应解析失败: %.200s", resp);
                err = ESP_ERR_INVALID_RESPONSE;
            }
        }
        free(resp);
    }
    if (err == ESP_OK) chat_history_add("assistant", out);   // 整轮成功才入历史
done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(body);
    return err;
}

// ---------------- audio/speech(流式) ----------------

esp_err_t ai_client_tts_stream(const char *text,
                               uint32_t *out_sample_rate, uint16_t *out_channels,
                               uint16_t *out_bits,
                               ai_tts_sink_t sink, void *user,
                               bool (*aborted)(void))
{
    char url[256];
    esp_err_t err = build_url(url, sizeof(url), "/audio/speech");
    if (err != ESP_OK) return err;

    // body = {"model":..,"input":"<转义文本>","voice":..,"response_format":"wav"}
    // input 可能含引号/换行,用 chat_core 的转义保持一致。
    size_t esc_cap = strlen(text) * 6 + 8;
    char *esc = malloc(esc_cap);
    if (!esc) return ESP_ERR_NO_MEM;
    size_t esc_len = chat_json_escape(esc, esc_cap, text);
    if (esc_len == 0) {
        free(esc);
        return ESP_ERR_INVALID_SIZE;
    }
    char body[1024];
    int bw = snprintf(body, sizeof(body),
                      "{\"model\":\"%s\",\"input\":%.*s,\"voice\":\"%s\",\"response_format\":\"wav\"}",
                      CONFIG_AI_CHAT_TTS_MODEL, (int)esc_len, esc, CONFIG_AI_CHAT_TTS_VOICE);
    free(esc);
    if (bw < 0 || (size_t)bw >= sizeof(body)) return ESP_ERR_INVALID_SIZE;

    esp_http_client_handle_t client = http_open(url);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    set_auth(client);

    err = ESP_FAIL;
    if (esp_http_client_open(client, (int)strlen(body)) != ESP_OK) {
        ESP_LOGE(TAG, "tts 连接失败");
        goto done;
    }
    if (esp_http_client_write(client, body, (int)strlen(body)) < 0) {
        ESP_LOGE(TAG, "tts 请求写失败");
        goto done;
    }
    esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200) {
        char errbody[256] = { 0 };
        int total = 0;
        while (total < (int)sizeof(errbody) - 1) {
            int n = esp_http_client_read(client, errbody + total, sizeof(errbody) - 1 - total);
            if (n <= 0) break;
            total += n;
        }
        ESP_LOGE(TAG, "tts HTTP %d: %.200s", esp_http_client_get_status_code(client), errbody);
        goto done;
    }

    {
        uint8_t *window = malloc(TTS_PROBE_WINDOW);
        uint8_t *rbuf = malloc(IO_CHUNK);
        if (!window || !rbuf) {
            free(window);
            free(rbuf);
            err = ESP_ERR_NO_MEM;
            goto done;
        }
        size_t win_len = 0;
        bool probed = false;
        err = ESP_OK;

        for (;;) {
            if (aborted && aborted()) break;      // 用户中止:关闭连接并返回 OK
            int n = esp_http_client_read(client, (char *)rbuf, IO_CHUNK);
            if (n < 0) {
                err = ESP_FAIL;
                break;
            }
            if (n == 0) break;                    // 响应读完
            if (!probed) {
                // 先攒够探测窗口:WAV 头(fmt/data 块)通过后才能确定采样参数开流。
                size_t space = TTS_PROBE_WINDOW - win_len;
                size_t take = (size_t)n < space ? (size_t)n : space;
                memcpy(window + win_len, rbuf, take);
                win_len += take;
                size_t off = chat_wav_probe(window, win_len,
                                            out_sample_rate, out_channels, out_bits);
                if (off > 0) {
                    probed = true;
                    sink(window + off, win_len - off, user);
                } else if (win_len == TTS_PROBE_WINDOW) {
                    ESP_LOGE(TAG, "tts 响应前 %u 字节内无 WAV 头(可能返回了 mp3)", (unsigned)win_len);
                    err = ESP_ERR_NOT_SUPPORTED;
                    break;
                }
            } else {
                sink(rbuf, (size_t)n, user);
            }
        }
        if (err == ESP_OK && !probed) {
            ESP_LOGE(TAG, "tts 流提前结束,未探测到 WAV 头");
            err = ESP_ERR_NOT_SUPPORTED;
        }
        free(window);
        free(rbuf);
    }
done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}
