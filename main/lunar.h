// main/lunar.h —— 公历↔农历换算、节日与法定节假日调休(纯逻辑,主机可测,禁依赖 IDF/LVGL)。
// 农历采用经典压缩位表(1900-2100);调休表覆盖 2026 年,2027+ 仅公历节日,农历节日照算。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- 公历日期(均按公历;year 1900..2100) ----

typedef struct {
    int year, month, day;
} solar_date_t;

// ---- 农历日期 ----

typedef struct {
    int year;          // 农历年份(干支纪年换算另用 lunar_ganzhi_year)
    int month;         // 1..12
    bool is_leap;      // 是否闰月
    int day;           // 1..30
} lunar_date_t;

// 公历→农历。越界/非法返回 false。
bool lunar_from_solar(solar_date_t solar, lunar_date_t *out);

// 农历月大小写(30=大月,29=小月);用于日历展示。越界返回 0。
int lunar_month_days(int lunar_year, int lunar_month, bool leap);

// 农历某年闰几月(0=无闰月)。
int lunar_leap_month(int lunar_year);

// 农历年干支(如 2026 丙午);"丙午"两字写入 out,返回 true。
bool lunar_ganzhi_year(int lunar_year, char *out, int cap);

// 农历月/日的中文表示:"正月"/"腊月"/"初一"/"十五"/"三十",写入 out。
bool lunar_month_name(const lunar_date_t *lunar, char *out, int cap);
bool lunar_day_name(const lunar_date_t *lunar, char *out, int cap);

// ---- 节日 ----

// 查当日节日名(可多个,以空格分隔写入 out;无节日返回 false)。
// 含公历节日(元旦/国庆等)、农历节日(春节/中秋/端午/七夕/重阳/腊八/元宵/除夕)。
bool lunar_festival_today(solar_date_t solar, char *out, int cap);

// ---- 2026 法定节假日/调休(国务院公布安排;2027+ 返回无特殊) ----

typedef enum {
    DAY_WORKDAY = 0,   // 普通工作日
    DAY_HOLIDAY,       // 法定休(放假)
    DAY_MAKEUP_WORK,   // 调休上班(周末上班)
} day_type_t;

// 判定某日类型:优先调休表,否则周末=非工作日(展示用,不等同法定)。
day_type_t lunar_day_type(solar_date_t solar, bool *is_weekend);

// 当日若是"休/班"角标文案("休"/"班"/""),写入 out。
bool lunar_day_badge(solar_date_t solar, char *out, int cap);

// 星期几(0=周日..6=周六);齐Berlin算法,无需历法表。
int solar_weekday(solar_date_t solar);

// 当月天数。
int solar_month_days(int year, int month);

#ifdef __cplusplus
}
#endif
