// main/page_weather.c —— 天气页:今日实况(温度/体感/湿度/风) + 未来三日预报。
// OK 短按=立即刷新(qweather_refresh_now);数据到达由外壳通知重绘。
#include "page_weather.h"

#include "app_pages.h"
#include "font_ai_chat_16.h"
#include "lunar.h"
#include "qweather.h"
#include "ui_pixel.h"

#include "bsp_display.h"
#include "lvgl.h"
#include "time_sync.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static lv_obj_t *s_scr;
static lv_obj_t *s_now_label;      // 实况多行
static lv_obj_t *s_daily_label;    // 三日多行
static lv_obj_t *s_batt;
static bool s_dirty;

static void redraw(void)
{
    if (!s_scr) return;
    qw_snapshot_t snap;
    qweather_get_snapshot(&snap);
    char buf[256];
    page_util_battery_text(s_batt);

    if (snap.fetched_at_sec == 0) {
        lv_label_set_text(s_now_label, "天气数据待更新...\n确认 WiFi 与和风配置后\n稍候片刻或按 OK 立即刷新");
        lv_label_set_text(s_daily_label, "");
        return;
    }
    snprintf(buf, sizeof(buf),
             "%s %d°C %s\n体感 %d°C  湿度 %d%%\n%s %d 级",
             snap.city, snap.now.temp, snap.now.text,
             snap.now.feels_like, snap.now.humidity,
             snap.now.wind_dir, snap.now.wind_scale);
    lv_label_set_text(s_now_label, buf);

    char out[192] = {0};
    size_t used = 0;
    static const char *kWeekZh[] = {"日", "一", "二", "三", "四", "五", "六"};
    for (int i = 0; i < snap.daily_count; i++) {
        const qw_daily_t *d = &snap.daily[i];
        int y, m, dd;
        char week[8] = "?";
        if (sscanf(d->date, "%d-%d-%d", &y, &m, &dd) == 3) {
            solar_date_t sd = {y, m, dd};
            snprintf(week, sizeof(week), "周%s", kWeekZh[solar_weekday(sd)]);
        }
        int n = snprintf(out + used, sizeof(out) - used,
                         "%s %s %d/%d°\n", week, d->text_day, d->temp_max, d->temp_min);
        if (n < 0 || used + (size_t)n >= sizeof(out)) break;
        used += (size_t)n;
    }
    lv_label_set_text(s_daily_label, out);
}

void page_weather_enter(void)
{
    s_scr = ui_pixel_screen_create("WEATHER");

    s_batt = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_batt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_batt, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_batt, LV_ALIGN_TOP_RIGHT, -34, 10);

    lv_obj_t *now_panel = ui_pixel_panel_create(s_scr, 12, 46, 216, 86, UI_PAPER);
    s_now_label = lv_label_create(now_panel);
    lv_obj_set_style_text_font(s_now_label, &font_ai_chat_16, 0);
    lv_obj_set_style_text_color(s_now_label, lv_color_hex(UI_INK), 0);
    lv_label_set_long_mode(s_now_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_now_label, 196);
    lv_obj_align(s_now_label, LV_ALIGN_TOP_LEFT, 8, 6);

    lv_obj_t *daily_panel = ui_pixel_panel_create(s_scr, 12, 142, 216, 130, UI_PAPER);
    s_daily_label = lv_label_create(daily_panel);
    lv_obj_set_style_text_font(s_daily_label, &font_ai_chat_16, 0);
    lv_obj_set_style_text_color(s_daily_label, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_daily_label, LV_ALIGN_TOP_LEFT, 8, 6);

    lv_obj_t *hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(hint, &font_ai_chat_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x5A6B7A), 0);
    lv_label_set_text(hint, "OK 立即刷新");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 280);

    redraw();
    lv_screen_load(s_scr);
}

void page_weather_exit(void)
{
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_now_label = s_daily_label = s_batt = NULL;
    }
}

void page_weather_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        qweather_refresh_now();
        if (s_now_label) {
            if (bsp_lvgl_lock(300)) {
                lv_label_set_text(s_now_label, "刷新中...");
                bsp_lvgl_unlock();
            }
        }
    }
}

const app_page_t page_weather_impl = {"WEATHER", page_weather_enter, page_weather_exit,
                                      page_weather_key};

void page_weather_notify_data(void)
{
    s_dirty = true;
}
