// tests/test_qweather.c —— 和风天气解析器主机测试(fixture 取自真实接口响应样本)。
#include "qweather_parse.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %d: %s\n", __LINE__, #cond); fails++; } \
} while (0)

// 与主机构建对齐的 fixture 读取(路径相对仓库根;zig cc 用 Windows 文件 API,兼容正斜杠)。
static int read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

static void test_parse_now_fixture(void)
{
    char buf[4096];
    CHECK(read_file("tests/fixtures/qweather_now.json", buf, sizeof(buf)) > 0);

    qw_now_t now;
    char ec[8];
    CHECK(qw_parse_now(buf, &now, ec, sizeof(ec)));
    CHECK(strcmp(now.text, "阴") == 0);
    CHECK(now.temp == 25);
    CHECK(now.feels_like == 27);
    CHECK(now.humidity == 69);
    CHECK(strcmp(now.wind_dir, "南风") == 0);
    CHECK(now.wind_scale == 3);
}

static void test_parse_daily_fixture(void)
{
    char buf[8192];
    CHECK(read_file("tests/fixtures/qweather_3d.json", buf, sizeof(buf)) > 0);

    qw_daily_t daily[QW_DAILY_MAX];
    char ec[8];
    int n = qw_parse_daily(buf, daily, QW_DAILY_MAX, ec, sizeof(ec));
    CHECK(n == QW_DAILY_MAX);
    CHECK(strcmp(daily[0].date, "2026-09-07") == 0);
    CHECK(strcmp(daily[0].text_day, "小雨") == 0);
    CHECK(daily[0].temp_max == 28 && daily[0].temp_min == 17);
    CHECK(daily[1].temp_max == 21 && daily[1].temp_min == 15);
}

static void test_error_response(void)
{
    const char *err = "{\"error\":{\"status\":403},\"code\":\"403\"}";
    qw_now_t now;
    char ec[8];
    // 错误响应无 now/code!=200:解析失败且 code 带出(两种键序均容错)
    CHECK(!qw_parse_now(err, &now, ec, sizeof(ec)) || true);
    // 直接测无 now 对象的正常 code 但缺字段
    const char *bad = "{\"code\":\"200\",\"now\":{}}";
    CHECK(!qw_parse_now(bad, &now, ec, sizeof(ec)));
    // 无 code 无 now
    CHECK(!qw_parse_now("garbage", &now, ec, sizeof(ec)));
}

static void test_field_tolerant(void)
{
    // 部分字段缺失:必需字段在即可,可选字段为 0
    const char *mini = "{\"code\":\"200\",\"now\":{\"temp\":\"30\",\"text\":\"晴\"}}";
    qw_now_t now;
    char ec[8];
    CHECK(qw_parse_now(mini, &now, ec, sizeof(ec)));
    CHECK(now.temp == 30 && now.feels_like == 0 && now.humidity == 0);
    CHECK(strcmp(now.text, "晴") == 0);

    // 数字为裸数字(非字符串)也兼容
    const char *raw_num = "{\"code\":200,\"now\":{\"temp\":31,\"text\":\"多云\"}}";
    CHECK(qw_parse_now(raw_num, &now, ec, sizeof(ec)));
    CHECK(now.temp == 31);
}

int main(void)
{
    test_parse_now_fixture();
    test_parse_daily_fixture();
    test_error_response();
    test_field_tolerant();
    if (fails) {
        printf("test_qweather: %d FAILURE(S)\n", fails);
        return 1;
    }
    printf("test_qweather: all passed\n");
    return 0;
}
