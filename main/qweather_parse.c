// main/qweather_parse.c —— 和风天气 JSON 解析(纯逻辑)。
// 扫描器采用原始视图迭代器:字符串按 [start,len) 视图比较(键均为 ASCII 标识符),
// 不复制、无长度上限,任意长字符串值(如 fxLink URL)不会使扫描失步。
// 命中键后值才做带转义的受控复制。字段一律容错:可选缺失填 0,必需缺失返回失败。
#include "qweather_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

// 找 p 起的下一个 JSON 字符串,给出内容视图(不含引号)与长度;返回结尾引号后位置。
// 转义对(\x)整体跳过,不会误判字符串边界。
static const char *next_string(const char *p, const char **start, size_t *len)
{
    while (*p && *p != '"') {
        if (*p == '\\') p++;
        p++;
    }
    if (*p != '"') return NULL;
    *start = ++p;
    while (*p && *p != '"') {
        if (*p == '\\') p++;
        p++;
    }
    if (*p != '"') return NULL;
    *len = (size_t)(p - *start);
    return p + 1;
}

static bool view_is(const char *start, size_t len, const char *key)
{
    return len == strlen(key) && memcmp(start, key, len) == 0;
}

// 带转义地把字符串视图复制到 out(容量安全;超长截断)。返回写入长度。
static size_t unescape_to(const char *s, size_t len, char *out, size_t cap)
{
    size_t used = 0;
    for (size_t i = 0; i < len && used + 1 < cap; i++) {
        char ch = s[i];
        if (ch == '\\' && i + 1 < len) {
            i++;
            switch (s[i]) {
            case 'n': ch = '\n'; break;
            case 't': ch = '\t'; break;
            case 'r': ch = '\r'; break;
            case '"': ch = '"'; break;
            case '\\': ch = '\\'; break;
            case '/': ch = '/'; break;
            case 'u': {                    // \uXXXX(含代理对)→ UTF-8
                unsigned cp = 0;
                int ok = 1;
                for (int k = 0; k < 4 && i + 1 < len; k++) {
                    i++;
                    char c = s[i];
                    if (c >= '0' && c <= '9')      cp = cp * 16 + (unsigned)(c - '0');
                    else if (c >= 'a' && c <= 'f') cp = cp * 16 + (unsigned)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') cp = cp * 16 + (unsigned)(c - 'A' + 10);
                    else { ok = 0; break; }
                }
                if (!ok) { out[used] = '\0'; return used; }
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < len &&
                    s[i + 1] == '\\' && s[i + 2] == 'u') {
                    unsigned lo = 0;
                    int ok2 = 1;
                    for (int k = 0; k < 4; k++) {
                        i += 2;
                        char c = s[i];
                        if (c >= '0' && c <= '9')      lo = lo * 16 + (unsigned)(c - '0');
                        else if (c >= 'a' && c <= 'f') lo = lo * 16 + (unsigned)(c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') lo = lo * 16 + (unsigned)(c - 'A' + 10);
                        else { ok2 = 0; break; }
                    }
                    if (ok2 && lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else
                        cp = 0xFFFD;
                } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                char u8[4];
                int ulen = 0;
                if (cp < 0x80) u8[ulen++] = (char)cp;
                else if (cp < 0x800) {
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
                for (int k = 0; k < ulen && used + 1 < cap; k++) out[used++] = u8[k];
                continue;
            }
            default: ch = s[i]; break;
            }
        }
        out[used++] = ch;
    }
    out[used] = '\0';
    return used;
}

// 迭代 json 中相邻的 "key": value,命中 key 时经回调返回值视图起点与类型。
typedef enum { V_STR, V_NUM, V_OBJ, V_ARR, V_OTHER } vtype_t;

// 找 "key" 的值:返回值起点与类型;未找到 NULL。
static const char *find_value(const char *json, const char *key, vtype_t *out_type)
{
    const char *p = json;
    while ((p = strchr(p, '"')) != NULL) {
        const char *kstart;
        size_t klen;
        const char *after = next_string(p, &kstart, &klen);
        if (!after) return NULL;
        const char *v = skip_ws(after);
        if (*v == ':' && view_is(kstart, klen, key)) {
            v = skip_ws(v + 1);
            if (*v == '\0') return NULL;
            if (*v == '"') { *out_type = V_STR; return v; }
            if (*v == '{') { *out_type = V_OBJ; return v; }
            if (*v == '[') { *out_type = V_ARR; return v; }
            *out_type = V_NUM;
            return v;
        }
        p = after;
    }
    return NULL;
}

static bool get_str(const char *json, const char *key, char *out, size_t cap)
{
    vtype_t t;
    const char *v = find_value(json, key, &t);
    if (!v || t != V_STR) return false;
    const char *start;
    size_t len;
    const char *end = next_string(v, &start, &len);
    if (!end) return false;
    unescape_to(start, len, out, cap);
    return true;
}

static bool get_int(const char *json, const char *key, int *out)
{
    vtype_t t;
    const char *v = find_value(json, key, &t);
    if (!v) return false;
    if (t == V_NUM) {
        char *end;
        long val = strtol(v, &end, 10);
        if (end == v) return false;
        *out = (int)val;
        return true;
    }
    if (t == V_STR) {                                  // "25" 形式
        const char *start;
        size_t len;
        const char *end = next_string(v, &start, &len);
        if (!end) return false;
        char buf[16];
        size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
        memcpy(buf, start, n);
        buf[n] = '\0';
        char *pend;
        long val = strtol(buf, &pend, 10);
        if (pend == buf) return false;
        *out = (int)val;
        return true;
    }
    return false;
}

static const char *get_object(const char *json, const char *key)
{
    vtype_t t;
    const char *v = find_value(json, key, &t);
    if (!v || (t != V_OBJ && t != V_ARR)) return NULL;
    return v + 1;                                      // 越过 '{' 或 '['
}

bool qw_parse_now(const char *json, qw_now_t *out, char *error_code, int ec_cap)
{
    if (error_code && ec_cap > 0) error_code[0] = '\0';
    if (!json || !out) return false;
    memset(out, 0, sizeof(*out));

    char code[8];
    if (get_str(json, "code", code, sizeof(code)) && strcmp(code, "200") != 0) {
        if (error_code) snprintf(error_code, (size_t)ec_cap, "%s", code);
        return false;
    }
    const char *now = get_object(json, "now");
    if (!now) return false;
    if (!get_str(now, "text", out->text, sizeof(out->text))) return false;
    if (!get_int(now, "temp", &out->temp)) return false;
    get_int(now, "feelsLike", &out->feels_like);
    get_int(now, "humidity", &out->humidity);
    get_str(now, "windDir", out->wind_dir, sizeof(out->wind_dir));
    get_int(now, "windScale", &out->wind_scale);
    return true;
}

int qw_parse_daily(const char *json, qw_daily_t *out, int max,
                   char *error_code, int ec_cap)
{
    if (error_code && ec_cap > 0) error_code[0] = '\0';
    if (!json || !out || max <= 0) return -1;

    char code[8];
    if (get_str(json, "code", code, sizeof(code)) && strcmp(code, "200") != 0) {
        if (error_code) snprintf(error_code, (size_t)ec_cap, "%s", code);
        return -1;
    }
    const char *daily = get_object(json, "daily");
    if (!daily) return -1;

    int count = 0;
    const char *p = daily;
    while (count < max && (p = strchr(p, '{')) != NULL) {
        const char *obj = p + 1;
        const char *end = strchr(obj, '}');
        if (!end) break;
        char fxdate[QW_DATE_MAX];
        if (get_str(obj, "fxDate", fxdate, sizeof(fxdate))) {
            qw_daily_t *d = &out[count];
            memset(d, 0, sizeof(*d));
            snprintf(d->date, sizeof(d->date), "%s", fxdate);
            get_str(obj, "textDay", d->text_day, sizeof(d->text_day));
            get_int(obj, "tempMax", &d->temp_max);
            get_int(obj, "tempMin", &d->temp_min);
            count++;
        }
        p = end;
    }
    return count;
}
