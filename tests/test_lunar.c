// tests/test_lunar.c —— lunar 纯逻辑主机测试:锚点日期 + 节日 + 2026 调休。
#include "lunar.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %d: %s\n", __LINE__, #cond); fails++; } \
} while (0)

static void test_solar_basics(void)
{
    CHECK(solar_month_days(2026, 2) == 28);
    CHECK(solar_month_days(2024, 2) == 29);
    CHECK(solar_month_days(2000, 2) == 29);
    solar_date_t d = {2026, 9, 6};
    CHECK(solar_weekday(d) == 0);            // 2026-09-06 是周日
    d.day = 7;
    CHECK(solar_weekday(d) == 1);
}

static void test_lunar_anchor_dates(void)
{
    lunar_date_t l;

    // 2026-02-17 = 丙午年正月初一(春节)
    solar_date_t chunjie = {2026, 2, 17};
    CHECK(lunar_from_solar(chunjie, &l));
    CHECK(l.year == 2026 && l.month == 1 && l.day == 1 && !l.is_leap);
    char gz[8], mn[8], dn[8];
    CHECK(lunar_ganzhi_year(l.year, gz, sizeof(gz)));
    CHECK(strcmp(gz, "丙午") == 0);
    CHECK(lunar_month_name(&l, mn, sizeof(mn)) && strcmp(mn, "正月") == 0);
    CHECK(lunar_day_name(&l, dn, sizeof(dn)) && strcmp(dn, "初一") == 0);

    // 2026-09-25 = 丙午年八月十五(中秋节)
    solar_date_t zhongqiu = {2026, 9, 25};
    CHECK(lunar_from_solar(zhongqiu, &l));
    CHECK(l.month == 8 && l.day == 15 && !l.is_leap);

    // 2025 乙巳蛇年:2025-01-29 为春节
    solar_date_t cj25 = {2025, 1, 29};
    CHECK(lunar_from_solar(cj25, &l));
    CHECK(l.year == 2025 && l.month == 1 && l.day == 1);
    CHECK(lunar_ganzhi_year(l.year, gz, sizeof(gz)) && strcmp(gz, "乙巳") == 0);

    // 2020 有闰四月(2020-05-23 为闰四月初一)
    solar_date_t leapday = {2020, 5, 23};
    CHECK(lunar_from_solar(leapday, &l));
    CHECK(l.year == 2020 && l.month == 4 && l.is_leap && l.day == 1);
    CHECK(lunar_leap_month(2020) == 4);
    CHECK(lunar_leap_month(2026) == 0);

    // 边界:1900-01-31 为基准日(庚子年正月初一);1900-01-30 属 1899 年(表外,应失败)
    solar_date_t base = {1900, 1, 31};
    CHECK(lunar_from_solar(base, &l));
    CHECK(l.year == 1900 && l.month == 1 && l.day == 1);
    solar_date_t before = {1899, 12, 31};
    CHECK(!lunar_from_solar(before, &l));
}

static void test_festivals(void)
{
    char buf[64];

    // 春节 + 公历无重叠
    solar_date_t d = {2026, 2, 17};
    CHECK(lunar_festival_today(d, buf, sizeof(buf)));
    CHECK(strcmp(buf, "春节") == 0);

    // 中秋
    d.month = 9; d.day = 25;
    CHECK(lunar_festival_today(d, buf, sizeof(buf)));
    CHECK(strcmp(buf, "中秋节") == 0);

    // 2026-02-16 为除夕(春节前一日)
    d.month = 2; d.day = 16;
    CHECK(lunar_festival_today(d, buf, sizeof(buf)));
    CHECK(strcmp(buf, "除夕") == 0);

    // 国庆 10/1
    d.month = 10; d.day = 1;
    CHECK(lunar_festival_today(d, buf, sizeof(buf)));
    CHECK(strcmp(buf, "国庆节") == 0);

    // 普通日无节日
    d.month = 3; d.day = 12;
    CHECK(!lunar_festival_today(d, buf, sizeof(buf)));
}

static void test_2026_holidays(void)
{
    char badge[4];
    bool weekend;

    // 春节 2/17(周二) = 休(带角标"休")
    solar_date_t d = {2026, 2, 17};
    CHECK(lunar_day_type(d, &weekend) == DAY_HOLIDAY && !weekend);
    CHECK(lunar_day_badge(d, badge, sizeof(badge)) && strcmp(badge, "休") == 0);

    // 2/14(周六) = 调休上班("班")
    d.day = 14;
    CHECK(lunar_day_type(d, &weekend) == DAY_MAKEUP_WORK);
    CHECK(lunar_day_badge(d, badge, sizeof(badge)) && strcmp(badge, "班") == 0);

    // 2/28(周六) = 班
    d.day = 28;
    CHECK(lunar_day_type(d, NULL) == DAY_MAKEUP_WORK);

    // 普通周六(2026-03-07)= 休但无角标(非法定)
    d.month = 3; d.day = 7;
    CHECK(lunar_day_type(d, &weekend) == DAY_HOLIDAY && weekend);
    CHECK(!lunar_day_badge(d, badge, sizeof(badge)));

    // 国庆 10/1 休;10/10(周六)班
    d.month = 10; d.day = 1;
    CHECK(lunar_day_type(d, NULL) == DAY_HOLIDAY);
    d.day = 10;
    CHECK(lunar_day_type(d, NULL) == DAY_MAKEUP_WORK);

    // 2025 年无表:2025-10-01 是周三,非周末,按普通工作日
    solar_date_t d25 = {2025, 10, 1};
    CHECK(lunar_day_type(d25, &weekend) == DAY_WORKDAY && !weekend);
    CHECK(!lunar_day_badge(d25, badge, sizeof(badge)));
}

int main(void)
{
    test_solar_basics();
    test_lunar_anchor_dates();
    test_festivals();
    test_2026_holidays();
    if (fails) {
        printf("test_lunar: %d FAILURE(S)\n", fails);
        return 1;
    }
    printf("test_lunar: all passed\n");
    return 0;
}
