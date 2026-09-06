// main/qweather_parse.h —— 和风天气响应解析(纯逻辑,主机可测,禁依赖 IDF/网络)。
// 输入为 gzip 解压后的 JSON 文本(和风 V7 响应强制 gzip,解压在传输层完成)。
// 手写轻量解析:只提取展示所需字段,不引入完整 JSON 库。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QW_TEXT_MAX   24     // 天气现象文案(中文短词)
#define QW_WIND_MAX   16     // 风向文案
#define QW_DATE_MAX   12     // "2026-09-07"
#define QW_DAILY_MAX  3      // 三日预报

typedef struct {
    char text[QW_TEXT_MAX];      // 天气现象(阴/晴/小雨...)
    int  temp;                   // 温度 °C
    int  feels_like;             // 体感 °C
    int  humidity;               // 湿度 %
    char wind_dir[QW_WIND_MAX];  // 风向(南风)
    int  wind_scale;             // 风力等级
} qw_now_t;

typedef struct {
    char date[QW_DATE_MAX];      // fxDate
    char text_day[QW_TEXT_MAX];
    int  temp_max, temp_min;
} qw_daily_t;

// /v7/weather/now 响应:取 code 与 now 对象字段。
// 成功(code=="200")返回 true;业务错误(403/402 等)返回 false 且 error_code 带出。
bool qw_parse_now(const char *json, qw_now_t *out, char *error_code, int ec_cap);

// /v7/weather/3d 响应:取 daily 数组前 min(QW_DAILY_MAX,n) 条,返回条数;负数=错误。
int qw_parse_daily(const char *json, qw_daily_t *out, int max,
                   char *error_code, int ec_cap);

#ifdef __cplusplus
}
#endif
