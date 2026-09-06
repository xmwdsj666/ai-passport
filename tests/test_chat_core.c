// tests/test_chat_core.c —— chat_core 主机测试(无需硬件,由 tools/validate.sh --static 编译运行)。
#include "chat_core.h"

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %d: %s\n", __LINE__, #cond); fails++; } \
} while (0)

static void test_state_machine_happy_path(void)
{
    chat_core_init();
    CHECK(chat_core_state() == CHAT_IDLE);
    chat_core_send(CHAT_EV_START_REC);
    CHECK(chat_core_state() == CHAT_RECORDING);
    chat_core_send(CHAT_EV_STOP_REC);
    CHECK(chat_core_state() == CHAT_RECOGNIZING);
    chat_history_add("user", "hi");
    chat_core_send(CHAT_EV_ASR_OK);
    CHECK(chat_core_state() == CHAT_THINKING);
    chat_history_add("assistant", "hello");
    chat_core_send(CHAT_EV_LLM_OK);
    CHECK(chat_core_state() == CHAT_SPEAKING);
    chat_core_send(CHAT_EV_TTS_OK);
    CHECK(chat_core_state() == CHAT_IDLE);
    CHECK(!chat_core_active());
}

static void test_state_machine_errors_and_interrupts(void)
{
    // 录音过短直接回空闲
    chat_core_init();
    chat_core_send(CHAT_EV_START_REC);
    chat_core_send(CHAT_EV_REC_TOO_SHORT);
    CHECK(chat_core_state() == CHAT_IDLE);

    // 各阶段失败进 ERROR,RESET 恢复
    const chat_event_t fail_evs[] = { CHAT_EV_REC_FAIL, CHAT_EV_ASR_FAIL,
                                      CHAT_EV_LLM_FAIL, CHAT_EV_TTS_FAIL };
    const chat_state_t pre[] = { CHAT_RECORDING, CHAT_RECOGNIZING,
                                 CHAT_THINKING, CHAT_SPEAKING };
    for (int i = 0; i < 4; i++) {
        chat_core_init();
        chat_core_send(CHAT_EV_START_REC);
        if (pre[i] != CHAT_RECORDING) chat_core_send(CHAT_EV_STOP_REC);
        if (pre[i] == CHAT_THINKING || pre[i] == CHAT_SPEAKING) chat_core_send(CHAT_EV_ASR_OK);
        if (pre[i] == CHAT_SPEAKING) chat_core_send(CHAT_EV_LLM_OK);
        CHECK(chat_core_state() == pre[i]);
        chat_core_send(fail_evs[i]);
        CHECK(chat_core_state() == CHAT_ERROR);
        CHECK(chat_core_error()[0] != '\0');
        chat_core_send(CHAT_EV_RESET);
        CHECK(chat_core_state() == CHAT_IDLE);
        CHECK(chat_core_error()[0] == '\0');
    }

    // 各阶段可被中断回空闲
    const chat_event_t inter_stage[] = { CHAT_EV_START_REC, CHAT_EV_STOP_REC,
                                         CHAT_EV_ASR_OK, CHAT_EV_LLM_OK };
    for (int i = 0; i < 4; i++) {
        chat_core_init();
        for (int j = 0; j <= i; j++) chat_core_send(inter_stage[j]);
        CHECK(chat_core_state() != CHAT_IDLE);
        chat_core_send(CHAT_EV_INTERRUPT);
        CHECK(chat_core_state() == CHAT_IDLE);
    }

    // 非法迁移忽略:空闲时收到 LLM_OK 不应变化也不崩溃
    chat_core_init();
    chat_core_send(CHAT_EV_LLM_OK);
    chat_core_send(CHAT_EV_TTS_OK);
    CHECK(chat_core_state() == CHAT_IDLE);

    // 错误态下直接按住说话可重新开始
    chat_core_init();
    chat_core_send(CHAT_EV_START_REC);
    chat_core_send(CHAT_EV_STOP_REC);
    chat_core_send(CHAT_EV_ASR_FAIL);
    CHECK(chat_core_state() == CHAT_ERROR);
    chat_core_send(CHAT_EV_START_REC);
    CHECK(chat_core_state() == CHAT_RECORDING);
    CHECK(chat_core_error()[0] == '\0');
}

static void test_history(void)
{
    chat_history_reset();
    CHECK(chat_history_count() == 0);
    CHECK(chat_history_text(0) == NULL);
    for (int i = 0; i < 10; i++) {
        char t[32];
        snprintf(t, sizeof(t), "msg-%d", i);
        chat_history_add(i % 2 ? "assistant" : "user", t);
    }
    CHECK(chat_history_count() == CHAT_HISTORY_MAX);
    // 丢最旧:第一条应为 msg-2
    CHECK(strcmp(chat_history_text(0), "msg-2") == 0);
    CHECK(strcmp(chat_history_text(CHAT_HISTORY_MAX - 1), "msg-9") == 0);
    CHECK(strcmp(chat_history_role(0), "user") == 0);
    CHECK(chat_history_role(CHAT_HISTORY_MAX) == NULL);

    // 超长文本截断且不以残缺字节结尾(内容含结尾 NUL)
    char big[CHAT_TEXT_MAX + 64];
    memset(big, 'a', sizeof(big));
    big[sizeof(big) - 1] = '\0';
    chat_history_add("user", big);
    CHECK(strlen(chat_history_text(CHAT_HISTORY_MAX - 1)) == CHAT_TEXT_MAX - 1);
}

static void test_build_json(void)
{
    char buf[1024];

    chat_history_reset();
    chat_history_add("user", "hi");
    char *out = chat_history_build_json(buf, sizeof(buf), "gpt-test", "be brief");
    CHECK(out == buf);
    if (out && strcmp(out, "{\"model\":\"gpt-test\",\"messages\":["
                          "{\"role\":\"system\",\"content\":\"be brief\"},"
                          "{\"role\":\"user\",\"content\":\"hi\"}]}") != 0) {
        printf("  actual: %s\n", out);
        fails++;
    }

    // 引号与换行转义
    chat_history_reset();
    chat_history_add("user", "say \"ok\"\nline2");
    out = chat_history_build_json(buf, sizeof(buf), "m", NULL);
    CHECK(out && strstr(out, "say \\\"ok\\\"\\nline2"));

    // 中文原样保留(UTF-8 不转义)
    chat_history_reset();
    chat_history_add("user", "你好");
    out = chat_history_build_json(buf, sizeof(buf), "m", "提示");
    CHECK(out && strstr(out, "你好") && strstr(out, "提示"));

    // 容量不足返回 NULL
    char tiny[32];
    chat_history_reset();
    chat_history_add("user", "a long enough message to overflow the tiny buffer");
    CHECK(chat_history_build_json(tiny, sizeof(tiny), "m", NULL) == NULL);
}

static void test_llm_parse(void)
{
    char out[CHAT_TEXT_MAX];

    // 正常结构 + 转义还原
    const char *r1 = "{\"choices\":[{\"index\":0,"
                     "\"message\":{\"role\":\"assistant\",\"content\":\"line1\\nline2\"},"
                     "\"finish_reason\":\"stop\"}]}";
    CHECK(chat_llm_parse_choice(r1, out, sizeof(out)) == out);
    CHECK(strcmp(out, "line1\nline2") == 0);

    // \uXXXX 与代理对(emoji)
    const char *r2 = "{\"choices\":[{\"message\":{\"content\":\"\\u4f60\\u597d \\ud83d\\ude00\"}}]}";
    CHECK(chat_llm_parse_choice(r2, out, sizeof(out)) == out);
    CHECK(strcmp(out, "你好 \xF0\x9F\x98\x80") == 0);

    // 缺字段
    CHECK(chat_llm_parse_choice("{\"object\":\"chat.completion\"}", out, sizeof(out)) == NULL);
    CHECK(chat_llm_parse_choice("{\"choices\":[]}", out, sizeof(out)) == NULL);
    CHECK(chat_llm_parse_choice("not json", out, sizeof(out)) == NULL);

    // 正文含 "content" 字样的长内容不应误导解析(键名扫描跳过字符串内部)
    const char *r3 = "{\"choices\":[{\"message\":{\"role\":\"assistant\","
                     "\"content\":\"the word content appears here: content is a word\"}}]}";
    CHECK(chat_llm_parse_choice(r3, out, sizeof(out)) == out);
    CHECK(strcmp(out, "the word content appears here: content is a word") == 0);

    // 超长内容截断安全
    char big[2048];
    snprintf(big, sizeof(big), "{\"choices\":[{\"message\":{\"content\":\"%s\"}}]}",
             "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    CHECK(chat_llm_parse_choice(big, out, sizeof(out)) == out);
}

static void test_asr_parse(void)
{
    char out[CHAT_TEXT_MAX];
    CHECK(chat_asr_parse_text("{\"text\":\"你好世界\"}", out, sizeof(out)) == out);
    CHECK(strcmp(out, "你好世界") == 0);
    CHECK(chat_asr_parse_text("{\"error\":\"x\"}", out, sizeof(out)) == NULL);
}

static void test_wav(void)
{
    uint8_t buf[256];
    uint32_t sr = 0;
    uint16_t ch = 0, bi = 0;

    CHECK(chat_wav_header(buf, 24000, 1, 16, 1000) == 44);
    CHECK(memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WAVE", 4) == 0);
    size_t off = chat_wav_probe(buf, 44 + 1000, &sr, &ch, &bi);
    CHECK(off == 44 && sr == 24000 && ch == 1 && bi == 16);

    // 非 WAV
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "OGGS", 4);
    CHECK(chat_wav_probe(buf, 100, &sr, &ch, &bi) == 0);

    // 头不完整
    CHECK(chat_wav_probe((const uint8_t *)"RIFF----WAVEfmt ", 16, &sr, &ch, &bi) == 0);

    // data 之前有附加块(LIST)也能探测:从头构造 RIFF+WAVE+fmt+LIST+data
    uint8_t wav2[128];
    memset(wav2, 0, sizeof(wav2));
    memcpy(wav2, "RIFF", 4);
    uint32_t riff_sz = 100, chunk_sz = 4;
    memcpy(wav2 + 4, &riff_sz, 4);
    memcpy(wav2 + 8, "WAVE", 4);
    memcpy(wav2 + 12, "fmt ", 4);                 // fmt 块:8 头 + 16 体
    uint32_t fmt_sz = 16;
    memcpy(wav2 + 16, &fmt_sz, 4);
    wav2[20] = 1;                                 // audioFormat = PCM
    wav2[22] = 1;                                 // channels = 1
    uint32_t rate = 16000;
    memcpy(wav2 + 24, &rate, 4);                  // sampleRate
    uint16_t bits = 16;
    wav2[34] = 16;                                // bitsPerSample
    (void)bits;
    memcpy(wav2 + 36, "LIST", 4);                 // LIST 块:8 头 + 4 体(含偶对齐)
    memcpy(wav2 + 40, &chunk_sz, 4);
    memcpy(wav2 + 48, "data", 4);
    uint32_t data_sz = 32;
    memcpy(wav2 + 52, &data_sz, 4);               // data 数据从 56 开始
    sr = ch = bi = 0;
    CHECK(chat_wav_probe(wav2, sizeof(wav2), &sr, &ch, &bi) == 56);
    CHECK(sr == 16000 && ch == 1 && bi == 16);
}

static void test_multipart(void)
{
    char buf[256];
    size_t n = chat_multipart_head(buf, sizeof(buf), "whisper-1", "audio.wav", 16000, 1000);
    CHECK(n > 0);
    CHECK(strstr(buf, "--" CHAT_MULTIPART_BOUNDARY "\r\n") == buf);
    CHECK(strstr(buf, "name=\"model\"\r\n\r\nwhisper-1\r\n") != NULL);
    CHECK(strstr(buf, "name=\"file\"; filename=\"audio.wav\"") != NULL);
    CHECK(strstr(buf, "Content-Type: audio/wav\r\n\r\n") != NULL);
    CHECK(buf[n - 1] == '\n');

    char tail[64];
    n = chat_multipart_tail(tail, sizeof(tail));
    // 收尾边界前必须有 CRLF(关闭前一个 part),且以 "--boundary--\r\n" 结束
    CHECK(n > 0 && strstr(tail, "--" CHAT_MULTIPART_BOUNDARY "--\r\n") == tail + 2);
}

int main(void)
{
    test_state_machine_happy_path();
    test_state_machine_errors_and_interrupts();
    test_history();
    test_build_json();
    test_llm_parse();
    test_asr_parse();
    test_wav();
    test_multipart();

    if (fails) {
        printf("test_chat_core: %d FAILURE(S)\n", fails);
        return 1;
    }
    printf("test_chat_core: all passed\n");
    return 0;
}
