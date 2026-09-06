// main/lunar.c —— 公历↔农历、节日、2026 调休(纯逻辑实现)。
// 农历压缩位表为公版通用数据(1900-2100),每年一个 20 位值:
//   bit16        闰月是否大月(30 天)
//   bit15..bit4  正月..十二月是否大月(30 天)
//   bit3..bit0   闰月月份(0=今年无闰月)
// 农历 1900 年正月初一 = 公历 1900-01-31,作为天数基准。
#include "lunar.h"

#include <stdio.h>
#include <string.h>

static const uint32_t kLunarTable[] = {
    0x04bd8, 0x04ae0, 0x0a570, 0x054d5, 0x0d260, 0x0d950, 0x16554, 0x056a0, 0x09ad0, 0x055d2, // 1900-1909
    0x04ae0, 0x0a5b6, 0x0a4d0, 0x0d250, 0x1d255, 0x0b540, 0x0d6a0, 0x0ada2, 0x095b0, 0x14977, // 1910-1919
    0x04970, 0x0a4b0, 0x0b4b5, 0x06a50, 0x06d40, 0x1ab54, 0x02b60, 0x09570, 0x052f2, 0x04970, // 1920-1929
    0x06566, 0x0d4a0, 0x0ea50, 0x06e95, 0x05ad0, 0x02b60, 0x186e3, 0x092e0, 0x1c8d7, 0x0c950, // 1930-1939
    0x0d4a0, 0x1d8a6, 0x0b550, 0x056a0, 0x1a5b4, 0x025d0, 0x092d0, 0x0d2b2, 0x0a950, 0x0b557, // 1940-1949
    0x06ca0, 0x0b550, 0x15355, 0x04da0, 0x0a5b0, 0x14573, 0x052b0, 0x0a9a8, 0x0e950, 0x06aa0, // 1950-1959
    0x0aea6, 0x0ab50, 0x04b60, 0x0aae4, 0x0a570, 0x05260, 0x0f263, 0x0d950, 0x05b57, 0x056a0, // 1960-1969
    0x096d0, 0x04dd5, 0x04ad0, 0x0a4d0, 0x0d4d4, 0x0d250, 0x0d558, 0x0b540, 0x0b6a0, 0x195a6, // 1970-1979
    0x095b0, 0x049b0, 0x0a974, 0x0a4b0, 0x0b27a, 0x06a50, 0x06d40, 0x0af46, 0x0ab60, 0x09570, // 1980-1989
    0x04af5, 0x04970, 0x064b0, 0x074a3, 0x0ea50, 0x06b58, 0x05ac0, 0x0ab60, 0x096d5, 0x092e0, // 1990-1999
    0x0c960, 0x0d954, 0x0d4a0, 0x0da50, 0x07552, 0x056a0, 0x0abb7, 0x025d0, 0x092d0, 0x0cab5, // 2000-2009
    0x0a950, 0x0b4a0, 0x0baa4, 0x0ad50, 0x055d9, 0x04ba0, 0x0a5b0, 0x15176, 0x052b0, 0x0a930, // 2010-2019
    0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60, 0x0a6e6, 0x0a4e0, 0x0d260, 0x0ea65, 0x0d530, // 2020-2029
    0x05aa0, 0x076a3, 0x096d0, 0x04afb, 0x04ad0, 0x0a4d0, 0x1d0b6, 0x0d250, 0x0d520, 0x0dd45, // 2030-2039
    0x0b5a0, 0x056d0, 0x055b2, 0x049b0, 0x0a577, 0x0a4b0, 0x0aa50, 0x1b255, 0x06d20, 0x0ada0, // 2040-2049
    0x14b63, 0x09370, 0x049f8, 0x04970, 0x064b0, 0x168a6, 0x0ea50, 0x06b20, 0x1a6c4, 0x0aae0, // 2050-2059
    0x0a2e0, 0x0d2e3, 0x0c960, 0x0d557, 0x0d4a0, 0x0da50, 0x05d55, 0x056a0, 0x0a6d0, 0x055d4, // 2060-2069
    0x052d0, 0x0a9b8, 0x0a950, 0x0b4a0, 0x0b6a6, 0x0ad50, 0x055a0, 0x0aba4, 0x0a5b0, 0x052b0, // 2070-2079
    0x0b273, 0x06930, 0x07337, 0x06aa0, 0x0ad50, 0x14b55, 0x04b60, 0x0a570, 0x054e4, 0x0d160, // 2080-2089
    0x0e968, 0x0d520, 0x0daa0, 0x16aa6, 0x056d0, 0x04ae0, 0x0a9d4, 0x0a2d0, 0x0d150, 0x0f252, // 2090-2099
    0x0d520,                                                                                  // 2100
};
#define LUNAR_FIRST_YEAR 1900
#define LUNAR_YEARS (int)(sizeof(kLunarTable) / sizeof(kLunarTable[0]))

static bool valid_solar(solar_date_t s)
{
    if (s.year < LUNAR_FIRST_YEAR || s.year >= LUNAR_FIRST_YEAR + LUNAR_YEARS) return false;
    if (s.month < 1 || s.month > 12 || s.day < 1) return false;
    return s.day <= solar_month_days(s.year, s.month);
}

int solar_month_days(int year, int month)
{
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    int d = kDays[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) d = 29;
    return d;
}

int solar_weekday(solar_date_t s)
{
    // Sakamoto 算法:返回 0=周日..6=周六
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = s.year;
    if (s.month < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[s.month - 1] + s.day) % 7;
}

// 公历日期 → 距农历基准日(1900-01-31)的天数。
static long solar_days_since_epoch(solar_date_t s)
{
    long days = 0;
    for (int y = LUNAR_FIRST_YEAR; y < s.year; y++) {
        days += 365 + ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0);
    }
    for (int m = 1; m < s.month; m++) days += solar_month_days(s.year, m);
    days += s.day - 1;
    return days - 30;   // 1900-01-01 与基准日 1900-01-31 相差 30 天
}

int lunar_leap_month(int lunar_year)
{
    if (lunar_year < LUNAR_FIRST_YEAR || lunar_year >= LUNAR_FIRST_YEAR + LUNAR_YEARS) return 0;
    return (int)(kLunarTable[lunar_year - LUNAR_FIRST_YEAR] & 0xF);
}

int lunar_month_days(int lunar_year, int lunar_month, bool leap)
{
    if (lunar_year < LUNAR_FIRST_YEAR || lunar_year >= LUNAR_FIRST_YEAR + LUNAR_YEARS) return 0;
    if (lunar_month < 1 || lunar_month > 12) return 0;
    uint32_t info = kLunarTable[lunar_year - LUNAR_FIRST_YEAR];
    if (leap) {
        if ((int)(info & 0xF) != lunar_month) return 0;
        return (info & 0x10000) ? 30 : 29;    // 闰月大小在 bit16
    }
    // bit15=正月,bit14=二月,...
    return (info & (0x8000u >> (lunar_month - 1))) ? 30 : 29;
}

static int lunar_year_days(int lunar_year)
{
    int sum = 0;
    for (int m = 1; m <= 12; m++) sum += lunar_month_days(lunar_year, m, false);
    int leap = lunar_leap_month(lunar_year);
    if (leap) sum += lunar_month_days(lunar_year, leap, true);
    return sum;
}

bool lunar_from_solar(solar_date_t solar, lunar_date_t *out)
{
    if (!out || !valid_solar(solar)) return false;
    long offset = solar_days_since_epoch(solar);
    int year = LUNAR_FIRST_YEAR;
    while (year < LUNAR_FIRST_YEAR + LUNAR_YEARS && offset >= lunar_year_days(year)) {
        offset -= lunar_year_days(year);
        year++;
    }
    if (year >= LUNAR_FIRST_YEAR + LUNAR_YEARS) return false;

    int leap = lunar_leap_month(year);
    int month = 1;
    bool is_leap = false;
    for (;;) {
        // 闰月排在同号常月之后:先扣常月,再查同号闰月
        int days = lunar_month_days(year, month, false);
        if (offset < days) {
            is_leap = false;
            break;
        }
        offset -= days;
        if (leap && month == leap) {
            int leap_days = lunar_month_days(year, month, true);
            if (offset < leap_days) {
                is_leap = true;
                break;
            }
            offset -= leap_days;
        }
        month++;
        if (month > 12) return false;   // 表数据异常,防御
    }
    out->year = year;
    out->month = month;
    out->is_leap = is_leap;
    out->day = (int)offset + 1;
    return true;
}

bool lunar_ganzhi_year(int lunar_year, char *out, int cap)
{
    static const char *gan[] = {"甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸"};
    static const char *zhi[] = {"子", "丑", "寅", "卯", "辰", "巳", "午", "未",
                                "申", "酉", "戌", "亥"};
    if (!out || cap < 3) return false;
    int g = ((lunar_year - 4) % 10 + 10) % 10;
    int z = ((lunar_year - 4) % 12 + 12) % 12;
    snprintf(out, cap, "%s%s", gan[g], zhi[z]);
    return true;
}

bool lunar_month_name(const lunar_date_t *lunar, char *out, int cap)
{
    static const char *names[] = {"正月", "二月", "三月", "四月", "五月", "六月",
                                  "七月", "八月", "九月", "十月", "冬月", "腊月"};
    if (!lunar || !out || cap < 5 || lunar->month < 1 || lunar->month > 12) return false;
    if (lunar->is_leap) snprintf(out, cap, "闰%s", names[lunar->month - 1]);
    else snprintf(out, cap, "%s", names[lunar->month - 1]);
    return true;
}

bool lunar_day_name(const lunar_date_t *lunar, char *out, int cap)
{
    static const char *names[] = {
        "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
        "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
        "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"};
    if (!lunar || !out || lunar->day < 1 || lunar->day > 30 || cap < 3) return false;
    snprintf(out, cap, "%s", names[lunar->day - 1]);
    return true;
}

// ---- 节日 ----

typedef struct {
    int month, day;
    const char *name;
} solar_festival_t;

static const solar_festival_t kSolarFestivals[] = {
    {1, 1, "元旦"}, {2, 14, "情人节"}, {3, 8, "妇女节"}, {4, 1, "愚人节"},
    {5, 1, "劳动节"}, {5, 4, "青年节"}, {6, 1, "儿童节"}, {9, 10, "教师节"},
    {10, 1, "国庆节"}, {10, 2, "国庆节"}, {10, 3, "国庆节"}, {12, 25, "圣诞节"},
};

typedef struct {
    int lunar_month, lunar_day;
    const char *name;
} lunar_festival_t;

static const lunar_festival_t kLunarFestivals[] = {
    {1, 1, "春节"}, {1, 15, "元宵节"}, {2, 2, "龙抬头"}, {5, 5, "端午节"},
    {7, 7, "七夕"}, {7, 15, "中元节"}, {8, 15, "中秋节"}, {9, 9, "重阳节"},
    {12, 8, "腊八节"},
};

static void append_name(char *out, int cap, int *used, const char *name)
{
    if (*used >= cap - 1) return;
    int n = snprintf(out + *used, cap - *used, "%s ", name);
    if (n > 0) *used += n;
}

bool lunar_festival_today(solar_date_t solar, char *out, int cap)
{
    if (!out || cap <= 0 || !valid_solar(solar)) return false;
    out[0] = '\0';
    int used = 0;
    for (size_t i = 0; i < sizeof(kSolarFestivals) / sizeof(kSolarFestivals[0]); i++) {
        if (kSolarFestivals[i].month == solar.month && kSolarFestivals[i].day == solar.day) {
            append_name(out, cap, &used, kSolarFestivals[i].name);
        }
    }
    lunar_date_t lunar;
    if (lunar_from_solar(solar, &lunar) && !lunar.is_leap) {
        // 除夕 = 公历次日恰为正月初一(含腊月廿九/三十两种年末形态)
        solar_date_t next = solar;
        next.day++;
        if (next.day > solar_month_days(next.year, next.month)) {
            next.day = 1;
            next.month++;
            if (next.month > 12) { next.month = 1; next.year++; }
        }
        lunar_date_t nl;
        if (lunar_from_solar(next, &nl) && !nl.is_leap && nl.month == 1 && nl.day == 1) {
            append_name(out, cap, &used, "除夕");
        }
        for (size_t i = 0; i < sizeof(kLunarFestivals) / sizeof(kLunarFestivals[0]); i++) {
            if (kLunarFestivals[i].lunar_month == lunar.month &&
                kLunarFestivals[i].lunar_day == lunar.day) {
                append_name(out, cap, &used, kLunarFestivals[i].name);
            }
        }
    }
    if (used > 0 && out[used - 1] == ' ') out[used - 1] = '\0';
    return out[0] != '\0';
}

// ---- 2026 法定节假日/调休(国务院办公厅公布的 2026 年安排) ----

typedef struct {
    int month, day;
    day_type_t type;
} special_day_t;

// 2026 年:元旦 1/1-1/3;春节 2/15-2/23(2/14、2/28 班);清明 4/4-4/6;
// 劳动节 5/1-5/5(4/26 班);端午 6/19-6/21;中秋 9/25-9/27;国庆 10/1-10/7(10/10 班)。
static const special_day_t k2026Special[] = {
    {1, 1, DAY_HOLIDAY}, {1, 2, DAY_HOLIDAY}, {1, 3, DAY_HOLIDAY},
    {2, 14, DAY_MAKEUP_WORK},
    {2, 15, DAY_HOLIDAY}, {2, 16, DAY_HOLIDAY}, {2, 17, DAY_HOLIDAY},
    {2, 18, DAY_HOLIDAY}, {2, 19, DAY_HOLIDAY}, {2, 20, DAY_HOLIDAY},
    {2, 21, DAY_HOLIDAY}, {2, 22, DAY_HOLIDAY}, {2, 23, DAY_HOLIDAY},
    {2, 28, DAY_MAKEUP_WORK},
    {4, 4, DAY_HOLIDAY}, {4, 5, DAY_HOLIDAY}, {4, 6, DAY_HOLIDAY},
    {4, 26, DAY_MAKEUP_WORK},
    {5, 1, DAY_HOLIDAY}, {5, 2, DAY_HOLIDAY}, {5, 3, DAY_HOLIDAY},
    {5, 4, DAY_HOLIDAY}, {5, 5, DAY_HOLIDAY},
    {6, 19, DAY_HOLIDAY}, {6, 20, DAY_HOLIDAY}, {6, 21, DAY_HOLIDAY},
    {9, 25, DAY_HOLIDAY}, {9, 26, DAY_HOLIDAY}, {9, 27, DAY_HOLIDAY},
    {10, 1, DAY_HOLIDAY}, {10, 2, DAY_HOLIDAY}, {10, 3, DAY_HOLIDAY},
    {10, 4, DAY_HOLIDAY}, {10, 5, DAY_HOLIDAY}, {10, 6, DAY_HOLIDAY},
    {10, 7, DAY_HOLIDAY}, {10, 10, DAY_MAKEUP_WORK},
};

day_type_t lunar_day_type(solar_date_t solar, bool *is_weekend)
{
    int wd = solar_weekday(solar);
    bool weekend = (wd == 0 || wd == 6);
    if (is_weekend) *is_weekend = weekend;
    if (solar.year == 2026) {
        for (size_t i = 0; i < sizeof(k2026Special) / sizeof(k2026Special[0]); i++) {
            if (k2026Special[i].month == solar.month && k2026Special[i].day == solar.day) {
                return k2026Special[i].type;
            }
        }
    }
    return weekend ? DAY_HOLIDAY : DAY_WORKDAY;
}

bool lunar_day_badge(solar_date_t solar, char *out, int cap)
{
    if (!out || cap < 1) return false;
    out[0] = '\0';
    if (solar.year != 2026) return false;   // 调休数据仅内置 2026
    bool weekend;
    day_type_t t = lunar_day_type(solar, &weekend);
    if (t == DAY_MAKEUP_WORK) snprintf(out, cap, "班");
    else if (t == DAY_HOLIDAY && !weekend) snprintf(out, cap, "休");
    return out[0] != '\0';
}
