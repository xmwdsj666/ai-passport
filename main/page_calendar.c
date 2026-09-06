// main/page_calendar.c —— 日历页:当月网格 + 今日高亮 + 农历小字 + 节日/休/班角标。
// 上下键翻月(回到当月时恢复高亮);网格 7 列 x 6 行,单元格 30x36。
#include "page_calendar.h"

#include "app_pages.h"
#include "font_ai_chat_16.h"
#include "lunar.h"
#include "ui_pixel.h"

#include "bsp_display.h"
#include "lvgl.h"
#include "time_sync.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define GRID_X0     12
#define GRID_Y0     76
#define CELL_W      31
#define CELL_H      37
#define CAL_WEEKS   6

static lv_obj_t *s_scr;
static lv_obj_t *s_title;          // "2026年9月"
static lv_obj_t *s_cells[CAL_WEEKS][7];
static int s_view_year, s_view_month;
static lv_timer_t *s_timer;        // 每分钟检查日期变化重绘

static const char *kWeekHeader[] = {"日", "一", "二", "三", "四", "五", "六"};

static bool same_date(int y, int m, int d)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    return time_sync_ready() &&
           tm_now.tm_year + 1900 == y && tm_now.tm_mon + 1 == m && tm_now.tm_mday == d;
}

static void build_cell_text(char *buf, size_t cap, int day)
{
    solar_date_t sd = {s_view_year, s_view_month, day};
    lunar_date_t lunar;
    char lunar_txt[8] = "";
    char fest[32];

    if (day == 1 && lunar_from_solar(sd, &lunar)) {
        // 每月 1 号显示农历月名
        char mn[8];
        if (lunar_month_name(&lunar, mn, sizeof(mn))) {
            snprintf(lunar_txt, sizeof(lunar_txt), "%s", mn);
        }
    } else if (lunar_from_solar(sd, &lunar)) {
        char dn[8];
        if (lunar_day_name(&lunar, dn, sizeof(dn))) {
            snprintf(lunar_txt, sizeof(lunar_txt), "%s", dn);
        }
    }
    char badge[4] = "";
    lunar_day_badge(sd, badge, sizeof(badge));
    if (lunar_festival_today(sd, fest, sizeof(fest))) {
        snprintf(buf, cap, "%d\n%s", day, fest);   // 节日优先于农历日
    } else if (badge[0]) {
        snprintf(buf, cap, "%d %s\n%s", day, badge, lunar_txt);
    } else {
        snprintf(buf, cap, "%d\n%s", day, lunar_txt);
    }
}

static void redraw(void)
{
    if (!s_scr) return;
    char title[24];
    snprintf(title, sizeof(title), "%d年%d月", s_view_year, s_view_month);
    lv_label_set_text(s_title, title);

    int first_wd = solar_weekday((solar_date_t){s_view_year, s_view_month, 1});
    int days = solar_month_days(s_view_year, s_view_month);

    for (int r = 0; r < CAL_WEEKS; r++) {
        for (int c = 0; c < 7; c++) {
            lv_obj_t *cell = s_cells[r][c];
            if (!cell) continue;
            int idx = r * 7 + c;
            int day = idx - first_wd + 1;
            if (day < 1 || day > days) {
                lv_label_set_text(cell, "");
                lv_obj_set_style_bg_color(cell, lv_color_hex(UI_PAPER), 0);
                continue;
            }
            char buf[48];
            build_cell_text(buf, sizeof(buf), day);
            lv_label_set_text(cell, buf);
            // 背景今日高亮,周末淡蓝
            solar_date_t sd = {s_view_year, s_view_month, day};
            int wd = solar_weekday(sd);
            if (same_date(s_view_year, s_view_month, day)) {
                lv_obj_set_style_bg_color(cell, lv_color_hex(UI_YELLOW), 0);
            } else if (wd == 0 || wd == 6) {
                lv_obj_set_style_bg_color(cell, lv_color_hex(UI_MUTED), 0);
            } else {
                lv_obj_set_style_bg_color(cell, lv_color_hex(UI_PAPER), 0);
            }
        }
    }
}

static void minute_tick(lv_timer_t *t)
{
    (void)t;
    // 跨月自动翻页(如 23:59 开着页面跨到次日 1 日)
    if (same_date(s_view_year, s_view_month, 1)) return;
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (time_sync_ready() &&
        (tm_now.tm_year + 1900 != s_view_year || tm_now.tm_mon + 1 != s_view_month)) {
        s_view_year = tm_now.tm_year + 1900;
        s_view_month = tm_now.tm_mon + 1;
        redraw();
    }
}

void page_calendar_enter(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (time_sync_ready()) {
        s_view_year = tm_now.tm_year + 1900;
        s_view_month = tm_now.tm_mon + 1;
    } else {
        s_view_year = 2026;
        s_view_month = 9;
    }

    s_scr = ui_pixel_screen_create("CALENDAR");

    s_title = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_title, &font_ai_chat_16, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 42);

    // 星期表头
    for (int c = 0; c < 7; c++) {
        lv_obj_t *h = lv_label_create(s_scr);
        lv_obj_set_style_text_font(h, &font_ai_chat_16, 0);
        lv_obj_set_style_text_color(h, lv_color_hex(0x5A6B7A), 0);
        lv_label_set_text(h, kWeekHeader[c]);
        lv_obj_align(h, LV_ALIGN_TOP_LEFT, GRID_X0 + c * CELL_W + 9, GRID_Y0 - 22);
    }

    // 网格单元:label 带背景小牌
    for (int r = 0; r < CAL_WEEKS; r++) {
        for (int c = 0; c < 7; c++) {
            lv_obj_t *cell = lv_label_create(s_scr);
            lv_obj_set_style_text_font(cell, &font_ai_chat_16, 0);
            lv_obj_set_style_text_color(cell, lv_color_hex(UI_INK), 0);
            lv_obj_set_style_pad_all(cell, 1, 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(cell, lv_color_hex(UI_PAPER), 0);
            lv_obj_set_style_radius(cell, 4, 0);
            lv_obj_set_pos(cell, GRID_X0 + c * CELL_W + 1, GRID_Y0 + r * CELL_H + 1);
            s_cells[r][c] = cell;
        }
    }

    lv_obj_t *hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(hint, &font_ai_chat_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x5A6B7A), 0);
    lv_label_set_text(hint, "上下键翻月");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 302);

    redraw();
    s_timer = lv_timer_create(minute_tick, 30000, NULL);
    lv_screen_load(s_scr);
}

void page_calendar_exit(void)
{
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_title = NULL;
        memset(s_cells, 0, sizeof(s_cells));
    }
}

void page_calendar_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        int step = (btn == BSP_BTN_UP) ? -1 : 1;
        s_view_month += step;
        if (s_view_month < 1) { s_view_month = 12; s_view_year--; }
        if (s_view_month > 12) { s_view_month = 1; s_view_year++; }
        redraw();
    }
}

const app_page_t page_calendar_impl = {"CALENDAR", page_calendar_enter,
                                       page_calendar_exit, page_calendar_key};
