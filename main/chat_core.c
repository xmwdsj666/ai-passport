// main/chat_core.c —— 纯逻辑实现:状态机 / 会话历史 / JSON 组装与解析 / WAV / multipart。
// 只依赖 C 标准库,保证主机测试与固件行为一致。
#include "chat_core.h"

#include <stdio.h>
#include <string.h>

// ---------------- 状态机 ----------------

static chat_state_t s_state = CHAT_IDLE;
static char s_error[96];

void chat_core_init(void)
{
    s_state = CHAT_IDLE;
    s_error[0] = '\0';
    chat_history_reset();
}

chat_state_t chat_core_state(void)
{
    return s_state;
}

const char *chat_core_state_name(chat_state_t st)
{
    switch (st) {
    case CHAT_IDLE:        return "IDLE";
    case CHAT_RECORDING:   return "RECORDING";
    case CHAT_RECOGNIZING: return "RECOGNIZING";
    case CHAT_THINKING:    return "THINKING";
    case CHAT_SPEAKING:    return "SPEAKING";
    case CHAT_ERROR:       return "ERROR";
    default:               return "?";
    }
}

const char *chat_core_error(void)
{
    return s_error;
}

static void set_error(const char *text)
{
    snprintf(s_error, sizeof(s_error), "%s", text ? text : "unknown");
}

bool chat_core_active(void)
{
    return s_state != CHAT_IDLE;
}

// 错误统一入口:进 ERROR 态并记录原因。
static void fail(chat_event_t ev)
{
    switch (ev) {
    case CHAT_EV_REC_FAIL: set_error("录音失败(内存不足?)"); break;
    case CHAT_EV_ASR_FAIL: set_error("识别失败,请重试"); break;
    case CHAT_EV_LLM_FAIL: set_error("生成回答失败,请重试"); break;
    case CHAT_EV_TTS_FAIL: set_error("播放失败"); break;
    default:               set_error("出错了"); break;
    }
    s_state = CHAT_ERROR;
}

void chat_core_send(chat_event_t ev)
{
    switch (s_state) {
    case CHAT_IDLE:
        if (ev == CHAT_EV_START_REC) {
            s_error[0] = '\0';                    // 新一轮开始时清除上次错误
            s_state = CHAT_RECORDING;
        } else if (ev == CHAT_EV_RESET) {
            set_error("");
        }
        break;                                    // 其余事件忽略(竞态安全)
    case CHAT_RECORDING:
        if (ev == CHAT_EV_STOP_REC) {
            s_state = CHAT_RECOGNIZING;
        } else if (ev == CHAT_EV_REC_TOO_SHORT || ev == CHAT_EV_INTERRUPT) {
            s_state = CHAT_IDLE;
        } else if (ev == CHAT_EV_REC_FAIL) {
            fail(ev);
        }
        break;
    case CHAT_RECOGNIZING:
        if (ev == CHAT_EV_ASR_OK) {
            s_state = CHAT_THINKING;
        } else if (ev == CHAT_EV_ASR_FAIL) {
            fail(ev);
        } else if (ev == CHAT_EV_INTERRUPT) {
            s_state = CHAT_IDLE;
        }
        break;
    case CHAT_THINKING:
        if (ev == CHAT_EV_LLM_OK) {
            s_state = CHAT_SPEAKING;
        } else if (ev == CHAT_EV_LLM_FAIL) {
            fail(ev);
        } else if (ev == CHAT_EV_INTERRUPT) {
            s_state = CHAT_IDLE;
        }
        break;
    case CHAT_SPEAKING:
        if (ev == CHAT_EV_TTS_OK) {
            s_state = CHAT_IDLE;
        } else if (ev == CHAT_EV_TTS_FAIL) {
            fail(ev);                             // 回答文本已入历史/气泡,只是没播出来
        } else if (ev == CHAT_EV_INTERRUPT) {
            s_state = CHAT_IDLE;
        }
        break;
    case CHAT_ERROR:
        if (ev == CHAT_EV_RESET || ev == CHAT_EV_START_REC) {
            s_error[0] = '\0';
            s_state = (ev == CHAT_EV_START_REC) ? CHAT_RECORDING : CHAT_IDLE;
        }
        break;
    }
}

// ---------------- 会话历史 ----------------

typedef struct {
    char role[12];
    char text[CHAT_TEXT_MAX];
} chat_msg_t;

static chat_msg_t s_msgs[CHAT_HISTORY_MAX];
static int s_count;   // 队列头固定为下标 0:满时丢最旧并前移

void chat_history_reset(void)
{
    s_count = 0;
}

int chat_history_count(void)
{
    return s_count;
}

const char *chat_history_role(int idx)
{
    if (idx < 0 || idx >= s_count) return NULL;
    return s_msgs[idx].role;
}

const char *chat_history_text(int idx)
{
    if (idx < 0 || idx >= s_count) return NULL;
    return s_msgs[idx].text;
}

void chat_history_add(const char *role, const char *text)
{
    if (!role || !text) return;
    if (s_count >= CHAT_HISTORY_MAX) {
        memmove(&s_msgs[0], &s_msgs[1], sizeof(chat_msg_t) * (CHAT_HISTORY_MAX - 1));
        s_count = CHAT_HISTORY_MAX - 1;
    }
    chat_msg_t *m = &s_msgs[s_count];
    snprintf(m->role, sizeof(m->role), "%s", role);
    snprintf(m->text, sizeof(m->text), "%s", text);
    s_count++;
}

// ---------------- JSON 工具(组装与解析) ----------------

// 把 text 以 JSON 字符串写入 out(含两端引号),返回写入字节数;容量不足返回 0。
// 非 ASCII 按 UTF-8 原样输出(ESP-IDF/OpenAI 兼容服务均接受 UTF-8)。
static size_t json_escape_write(char *out, size_t cap, const char *text)
{
    size_t used = 0;
    if (cap < 2) return 0;
    out[used++] = '"';
    for (const char *p = text; *p; p++) {
        char esc;
        switch (*p) {
        case '"':  esc = '"';  goto escape;
        case '\\': esc = '\\'; goto escape;
        case '\n': esc = 'n';  goto escape;
        case '\r': esc = 'r';  goto escape;
        case '\t': esc = 't';  goto escape;
        case '\b': esc = 'b';  goto escape;
        case '\f': esc = 'f';  goto escape;
        default:
            if ((unsigned char)*p < 0x20) {
                if (used + 6 >= cap) return 0;
                used += (size_t)snprintf(out + used, cap - used, "\\u%04x", *p);
            } else {
                if (used + 1 >= cap) return 0;
                out[used++] = *p;
            }
            continue;
        }
escape:
        if (used + 2 >= cap) return 0;
        out[used++] = '\\';
        out[used++] = esc;
    }
    if (used + 1 >= cap) return 0;
    out[used++] = '"';
    return used;
}

size_t chat_json_escape(char *out, size_t cap, const char *text)
{
    return json_escape_write(out, cap, text ? text : "");
}

char *chat_history_build_json(char *buf, size_t cap,
                              const char *model, const char *system_prompt)
{
    if (!buf || cap < 64) return NULL;
    size_t used = 0;
    used += (size_t)snprintf(buf + used, cap - used, "{\"model\":");
    size_t n = json_escape_write(buf + used, cap - used, model ? model : "");
    if (!n) return NULL;
    used += n;
    used += (size_t)snprintf(buf + used, cap - used, ",\"messages\":[");

    if (system_prompt && system_prompt[0]) {
        used += (size_t)snprintf(buf + used, cap - used, "{\"role\":\"system\",\"content\":");
        n = json_escape_write(buf + used, cap - used, system_prompt);
        if (!n) return NULL;
        used += n;
        if (used + 2 >= cap) return NULL;         // 需容下对象收尾 '}' 与可能的 ','
        buf[used++] = '}';                        // system 消息对象在此闭合
        if (s_count > 0) buf[used++] = ',';
    }
    for (int i = 0; i < s_count; i++) {
        used += (size_t)snprintf(buf + used, cap - used, "{\"role\":");
        n = json_escape_write(buf + used, cap - used, s_msgs[i].role);
        if (!n) return NULL;
        used += n;
        used += (size_t)snprintf(buf + used, cap - used, ",\"content\":");
        n = json_escape_write(buf + used, cap - used, s_msgs[i].text);
        if (!n) return NULL;
        used += n;
        if (used + 3 >= cap) return NULL;
        buf[used++] = '}';
        if (i + 1 < s_count) buf[used++] = ',';
    }
    used += (size_t)snprintf(buf + used, cap - used, "]}");
    if (used >= cap) return NULL;
    return buf;
}

// 跳过空白。
static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

// 从 p 处解析 JSON 字符串字面量(已确认是 '"' 开头),处理转义(含 \uXXXX 与代理对)。
// 返回字符串结束后的位置(结尾引号之后);解析失败返回 NULL。
static const char *json_read_string(const char *p, char *out, size_t cap)
{
    size_t used = 0;
    p++;                                          // 跳过开头引号
    while (*p && *p != '"') {
        char ch = *p;
        if (ch == '\\') {
            p++;
            switch (*p) {
            case 'n': ch = '\n'; p++; break;
            case 't': ch = '\t'; p++; break;
            case 'r': ch = '\r'; p++; break;
            case 'b': ch = '\b'; p++; break;
            case 'f': ch = '\f'; p++; break;
            case '"': ch = '"';  p++; break;
            case '\\': ch = '\\'; p++; break;
            case '/': ch = '/';  p++; break;
            case 'u': {
                unsigned cp = 0;
                for (int i = 0; i < 4; i++) {
                    p++;
                    char c = *p;
                    if (c >= '0' && c <= '9')      cp = cp * 16 + (unsigned)(c - '0');
                    else if (c >= 'a' && c <= 'f') cp = cp * 16 + (unsigned)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') cp = cp * 16 + (unsigned)(c - 'A' + 10);
                    else return NULL;
                }
                p++;
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    unsigned lo = 0;              // 高代理:尝试读低代理
                    int ok = 1;
                    p += 2;
                    for (int i = 0; i < 4; i++) {
                        char c = p[i];
                        if (c >= '0' && c <= '9')      lo = lo * 16 + (unsigned)(c - '0');
                        else if (c >= 'a' && c <= 'f') lo = lo * 16 + (unsigned)(c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') lo = lo * 16 + (unsigned)(c - 'A' + 10);
                        else { ok = 0; break; }
                    }
                    if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 4;
                    } else {
                        cp = 0xFFFD;              // 无效代理对,替换符
                    }
                } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                char u8[4];
                int ulen = 0;
                if (cp < 0x80) {
                    u8[ulen++] = (char)cp;
                } else if (cp < 0x800) {
                    u8[ulen++] = (char)(0xC0 | (cp >> 6));
                    u8[ulen++] = (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    u8[ulen++] = (char)(0xE0 | (cp >> 12));
                    u8[ulen++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    u8[ulen++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    u8[ulen++] = (char)(0xF0 | (cp >> 18));
                    u8[ulen++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    u8[ulen++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    u8[ulen++] = (char)(0x80 | (cp & 0x3F));
                }
                for (int i = 0; i < ulen; i++) {
                    if (used + 1 >= cap) return NULL;
                    out[used++] = u8[i];
                }
                continue;
            }
            default: return NULL;
            }
        } else {
            p++;
        }
        if (used + 1 >= cap) return NULL;
        out[used++] = ch;
    }
    if (*p != '"') return NULL;
    out[used] = '\0';
    return p + 1;
}

// 逐个字符串读出并与 key 比对;命中且其后是 ':' 时读出字符串值(写入 out)。
// 返回值起点;未命中返回 NULL。扫描时正确跳过字符串内部,
// 避免把正文里恰好出现的 "content" 等词误当键名。
static const char *json_find_string_value(const char *json, const char *key,
                                          char *out, size_t cap)
{
    size_t klen = strlen(key);
    char tmp[48];
    const char *p = json;
    while ((p = strchr(p, '"')) != NULL) {
        const char *after = json_read_string(p, tmp, sizeof(tmp));
        if (!after) { p += 1; continue; }         // 字符串本身非法(截断),跳一个引号继续
        const char *q = skip_ws(after);
        if (strlen(tmp) == klen && strncmp(tmp, key, klen) == 0 && *q == ':') {
            q = skip_ws(q + 1);
            if (*q == '"') {
                const char *vend = json_read_string(q, out, cap);
                return vend ? q : NULL;           // 返回值起点(便于调用方判断)
            }
            return NULL;                          // 值不是字符串
        }
        p = after;
    }
    return NULL;
}

// 嵌套定位:依次要求每个 key 的值是对象并深入,最后取 leaf 字符串值。
// 返回 out(成功)或 NULL。
static char *json_path_string(const char *json, const char **keys, int nkeys,
                              char *out, size_t cap)
{
    const char *p = json;
    for (int i = 0; i < nkeys - 1; i++) {
        // 找到 keys[i] 的字符串键名,确认其后是 '{'
        char tmp[48];
        const char *q = p;
        const char *found = NULL;
        while ((q = strchr(q, '"')) != NULL) {
            const char *after = json_read_string(q, tmp, sizeof(tmp));
            if (!after) { q += 1; continue; }
            const char *v = skip_ws(after);
            if (strlen(tmp) == strlen(keys[i]) && strncmp(tmp, keys[i], strlen(keys[i])) == 0
                && *v == ':') {
                found = skip_ws(v + 1);
                break;
            }
            q = after;
        }
        if (!found || *found != '{') return NULL;
        p = found + 1;
    }
    return json_find_string_value(p, keys[nkeys - 1], out, cap) ? out : NULL;
}

char *chat_llm_parse_choice(const char *json, char *out, size_t cap)
{
    if (!json || !out || cap == 0) return NULL;
    // choices[0].message.content;首个 choices 对象即 choices[0]
    const char *keys[] = { "choices", "message", "content" };
    const char *c = strstr(json, "\"choices\"");
    if (!c) return NULL;
    const char *brk = strchr(c, '[');
    if (!brk) return NULL;
    const char *obj = strchr(brk, '{');
    if (!obj) return NULL;
    return json_path_string(obj, keys + 1, 2, out, cap);   // message.content
}

char *chat_asr_parse_text(const char *json, char *out, size_t cap)
{
    if (!json || !out || cap == 0) return NULL;
    return json_find_string_value(json, "text", out, cap) ? out : NULL;
}

// ---------------- WAV ----------------

static void wav_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void wav_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static uint32_t wav_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t wav_rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

size_t chat_wav_header(uint8_t out[44], uint32_t sample_rate,
                       uint16_t channels, uint16_t bits, uint32_t data_bytes)
{
    if (!out || channels == 0 || bits == 0 || sample_rate == 0) return 0;
    uint32_t byte_rate = sample_rate * channels * (bits / 8);
    uint16_t block_align = (uint16_t)(channels * (bits / 8));
    memcpy(out, "RIFF", 4);
    wav_le32(out + 4, 36 + data_bytes);
    memcpy(out + 8, "WAVE", 4);
    memcpy(out + 12, "fmt ", 4);
    wav_le32(out + 16, 16);
    wav_le16(out + 20, 1);                        // PCM
    wav_le16(out + 22, channels);
    wav_le32(out + 24, sample_rate);
    wav_le32(out + 28, byte_rate);
    wav_le16(out + 32, block_align);
    wav_le16(out + 34, bits);
    memcpy(out + 36, "data", 4);
    wav_le32(out + 40, data_bytes);
    return 44;
}

size_t chat_wav_probe(const uint8_t *data, size_t len,
                      uint32_t *sample_rate, uint16_t *channels, uint16_t *bits)
{
    if (!data || len < 44) return 0;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) return 0;
    const uint8_t *p = data + 12;
    const uint8_t *end = data + len;
    uint32_t sr = 0;
    uint16_t ch = 0, bi = 0;
    while (p + 8 <= end) {
        uint32_t sz = wav_rd32(p + 4);
        if (memcmp(p, "fmt ", 4) == 0 && p + 8 + 16 <= end) {
            wav_rd16(p + 8);                      // audioFormat(PCM=1),暂不校验
            ch = wav_rd16(p + 10);
            sr = wav_rd32(p + 12);
            bi = wav_rd16(p + 22);
        } else if (memcmp(p, "data", 4) == 0) {
            if (sr == 0 || ch == 0 || bi == 0) return 0;   // fmt 必须在 data 之前且完整
            if (sample_rate) *sample_rate = sr;
            if (channels) *channels = ch;
            if (bits) *bits = bi;
            return (size_t)(p + 8 - data);
        }
        p += 8 + sz + (sz & 1);                   // 块按 2 字节对齐
    }
    return 0;                                     // data 块头还没到齐,让调用方继续缓冲
}

// ---------------- multipart ----------------

size_t chat_multipart_head(char *buf, size_t cap, const char *model,
                           const char *filename, uint32_t sample_rate, uint32_t pcm_bytes)
{
    uint8_t wav[44];
    if (chat_wav_header(wav, sample_rate, 1, 16, (uint32_t)pcm_bytes) != 44) return 0;
    // OpenAI 兼容 /audio/transcriptions 要求 model 表单字段,放在 file part 之前
    int n = snprintf(buf, cap,
                     "--%s\r\n"
                     "Content-Disposition: form-data; name=\"model\"\r\n"
                     "\r\n"
                     "%s\r\n"
                     "--%s\r\n"
                     "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                     "Content-Type: audio/wav\r\n"
                     "\r\n",
                     CHAT_MULTIPART_BOUNDARY, model ? model : "",
                     CHAT_MULTIPART_BOUNDARY, filename ? filename : "audio.wav");
    if (n < 0 || (size_t)n + 1 > cap) return 0;
    return (size_t)n;
}

size_t chat_multipart_tail(char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "\r\n--%s--\r\n", CHAT_MULTIPART_BOUNDARY);
    if (n < 0 || (size_t)n + 1 > cap) return 0;
    return (size_t)n;
}
