// main/page_home.c —— 主页:大时钟 + 日期/星期/农历/节日 + 天气摘要 + 电池。
// 数据来源:系统时间(time_sync 后有效)、lunar 纯逻辑、qweather 快照。
// 页面持有 1s LVGL 定时器刷新时钟;数据回调经外壳通知时即时重绘。
#include "page_home.h"

#include "app_pages.h"
#include "font_ai_chat_16.h"
#include "lunar.h"
#include "qweather.h"
#include "ui_pixel.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "lvgl.h"
#include "time_sync.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "page_home";

static lv_obj_t *s_scr;
static lv_obj_t *s_clock_label;
static lv_obj_t *s_date_label;     // 日期 + 星期
static lv_obj_t *s_lunar_label;    // 农历 + 节日
static lv_obj_t *s_weather_label;  // 天气摘要
static lv_obj_t *s_batt;
static lv_timer_t *s_timer;
static bool s_dirty;               // 天气/时间数据变化,下个 tick 重绘

static const char *kWeekZh[] = {"日", "一", "二", "三", "四", "五", "六"};

static void battery_refresh(void)
{
    if (!s_batt) return;
    page_util_battery_text(s_batt);
}

// 依据系统时间重绘全部文本(时间无效时给出未对时提示)。
static void redraw(void)
{
    if (!s_scr) return;
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    bool time_valid = time_sync_ready() && tm_now.tm_year >= 126;   // 2026+ 才可信

    char buf[96];

    if (time_valid) {
        snprintf(buf, sizeof(buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        lv_label_set_text(s_clock_label, buf);

        solar_date_t solar = {tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday};
        snprintf(buf, sizeof(buf), "%d月%d日 星期%s", solar.month, solar.day,
                 kWeekZh[solar_weekday(solar)]);
        lv_label_set_text(s_date_label, buf);

        lunar_date_t lunar;
        char lunar_txt[64] = {0}, mn[8], dn[8];
        if (lunar_from_solar(solar, &lunar) &&
            lunar_month_name(&lunar, mn, sizeof(mn)) &&
            lunar_day_name(&lunar, dn, sizeof(dn))) {
            snprintf(lunar_txt, sizeof(lunar_txt), "%s%s", mn, dn);
        }
        char fest[48];
        if (lunar_festival_today(solar, fest, sizeof(fest))) {
            size_t len = strlen(lunar_txt);
            snprintf(lunar_txt + len, sizeof(lunar_txt) - len, " | %.14s", fest);
        }
        lv_label_set_text(s_lunar_label, lunar_txt);

        // 天气摘要:快照可能为空(尚无数据),给出优雅文案
        qw_snapshot_t snap;
        qweather_get_snapshot(&snap);
        if (snap.fetched_at_sec > 0) {
            snprintf(buf, sizeof(buf), "%s %d°%s",
                     snap.city, snap.now.temp, snap.now.text);
        } else {
            snprintf(buf, sizeof(buf), "%s 天气待更新", CONFIG_QWEATHER_LOCATION_NAME);
        }
        lv_label_set_text(s_weather_label, buf);
    } else {
        lv_label_set_text(s_clock_label, "--:--");
        lv_label_set_text(s_date_label, "等待网络对时...");
        lv_label_set_text(s_lunar_label, "");
        lv_label_set_text(s_weather_label, "");
    }
    battery_refresh();
}

static void tick(lv_timer_t *t)
{
    (void)t;
    if (s_dirty) {
        s_dirty = false;
        redraw();
        return;
    }
    // 每秒刷时钟(仅时分变化时重绘,减少无效绘制)
    if (s_clock_label && time_sync_ready()) {
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        if (lv_label_get_text(s_clock_label) &&
            strcmp(lv_label_get_text(s_clock_label), buf) != 0) {
            lv_label_set_text(s_clock_label, buf);
        }
    }
}

static void on_weather_update(void *user)
{
    (void)user;
    s_dirty = true;                      // worker 上下文:仅置位,定时器里刷 UI
}

void page_home_enter(void)
{
    s_scr = ui_pixel_screen_create("HOME");
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(UI_SKY), 0);

    // 电池(右上,避开云朵装饰)
    s_batt = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_batt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_batt, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_batt, LV_ALIGN_TOP_RIGHT, -34, 10);

    // 大时钟(纸面牌)
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 26, 52, 188, 74, UI_PAPER);
    s_clock_label = lv_label_create(panel);
    lv_obj_set_style_text_font(s_clock_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_clock_label, lv_color_hex(UI_INK), 0);
    lv_obj_center(s_clock_label);

    // 日期行
    s_date_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_date_label, &font_ai_chat_16, 0);
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_date_label, LV_ALIGN_TOP_MID, 0, 138);

    // 农历/节日行
    s_lunar_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_lunar_label, &font_ai_chat_16, 0);
    lv_obj_set_style_text_color(s_lunar_label, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_lunar_label, LV_ALIGN_TOP_MID, 0, 162);

    // 天气摘要牌
    lv_obj_t *wpanel = ui_pixel_panel_create(s_scr, 26, 196, 188, 44, UI_PAPER);
    s_weather_label = lv_label_create(wpanel);
    lv_obj_set_style_text_font(s_weather_label, &font_ai_chat_16, 0);
    lv_obj_set_style_text_color(s_weather_label, lv_color_hex(UI_INK), 0);
    lv_obj_center(s_weather_label);

    // 提示行
    lv_obj_t *hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(hint, &font_ai_chat_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x5A6B7A), 0);
    lv_label_set_text(hint, "上下键翻页 · OK 长按回主页");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 252);

    redraw();
    s_timer = lv_timer_create(tick, 1000, NULL);
    lv_screen_load(s_scr);
}

void page_home_exit(void)
{
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_clock_label = s_date_label = s_lunar_label = NULL;
        s_weather_label = s_batt = NULL;
    }
}

void page_home_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    (void)btn;
    (void)ev;                            // 主页无页内按键动作
}

const app_page_t page_home_impl = {"HOME", page_home_enter, page_home_exit, page_home_key};

// ---- 共享小工具:电池文本(各页复用) ----
void page_util_battery_text(lv_obj_t *label)
{
    if (!label) return;
    int soc = bsp_battery_soc();
    if (soc < 0) lv_label_set_text(label, LV_SYMBOL_BATTERY_EMPTY " --");
    else lv_label_set_text_fmt(label, LV_SYMBOL_BATTERY_FULL " %d%%", soc);
}

// 外壳在天气/时间数据变化时调用:置脏标记,页面定时器负责重绘(线程安全)。
void page_home_notify_data(void)
{
    s_dirty = true;
}
