// main/ui_chat.c —— 对话页界面实现。
// 动态文本统一用 CJK 字体(simsun 16,约 1000 常用字);生僻字会缺字形,
// 通过系统提示词约束 LLM 用字范围缓解(见 Kconfig AI_CHAT_SYSTEM_PROMPT)。
#include "ui_chat.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "ui_pixel.h"

#include <stdio.h>
#include <string.h>

#define CHAT_AREA_X    12
#define CHAT_AREA_Y    48
#define CHAT_AREA_W    216
#define CHAT_AREA_H    176
#define BUBBLE_MAX_W   164
#define STATUS_PANEL_Y 230

#if LV_FONT_SIMSUN_16_CJK
#define FONT_CJK &lv_font_simsun_16_cjk
#else
// 未启用 CJK 字体时退回 Montserrat(中文会缺字形),构建不被卡死
#define FONT_CJK &lv_font_montserrat_14
#warning "Enable CONFIG_LV_FONT_SIMSUN_16_CJK for Chinese text"
#endif

static lv_obj_t *s_scr;          // 页面屏(归 demo_ai_chat 所有)
static lv_obj_t *s_scroll;       // 气泡滚动容器
static lv_obj_t *s_status;       // 状态行
static lv_obj_t *s_net;          // 网络行
static lv_obj_t *s_batt;         // 电池
static int s_msg_count;

// ---- 电池(右上被云朵装饰占用,放左上天空区;读数不可用时优雅降级) ----

static void battery_refresh(void)
{
    if (!s_batt) return;
    int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_label_set_text(s_batt, LV_SYMBOL_BATTERY_EMPTY " --");
    } else {
        lv_label_set_text_fmt(s_batt, LV_SYMBOL_BATTERY_FULL " %d%%", soc);
    }
}

// ---- 气泡 ----

static lv_obj_t *add_bubble(const char *text, bool user)
{
    // 每条消息一行(透明行容器),气泡按角色靠左/靠右
    lv_obj_t *row = lv_obj_create(s_scroll);
    lv_obj_set_size(row, CHAT_AREA_W - 12, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bub = lv_obj_create(row);
    lv_obj_set_width(bub, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(bub, BUBBLE_MAX_W, 0);
    lv_obj_set_style_bg_color(bub, lv_color_hex(user ? UI_MUTED : UI_PAPER), 0);
    lv_obj_set_style_border_color(bub, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(bub, 1, 0);
    lv_obj_set_style_radius(bub, 8, 0);
    lv_obj_set_style_pad_all(bub, 6, 0);

    lv_obj_t *lab = lv_label_create(bub);
    lv_obj_set_style_text_font(lab, FONT_CJK, 0);
    lv_obj_set_style_text_color(lab, lv_color_hex(UI_INK), 0);
    lv_obj_set_width(lab, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(lab, BUBBLE_MAX_W - 12, 0);
    lv_label_set_long_mode(lab, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lab, text);

    lv_obj_align(bub, user ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, 0, 0);
    return row;
}

// ---- 状态文案 ----

static void status_apply(chat_state_t st, const char *error)
{
    if (!s_status) return;
    switch (st) {
    case CHAT_IDLE:        lv_label_set_text(s_status, "按住【下】键说话"); break;
    case CHAT_RECORDING:   lv_label_set_text(s_status, "录音中... 松开结束"); break;
    case CHAT_RECOGNIZING: lv_label_set_text(s_status, "识别中..."); break;
    case CHAT_THINKING:    lv_label_set_text(s_status, "思考中..."); break;
    case CHAT_SPEAKING:    lv_label_set_text(s_status, "播放中... OK 停止"); break;
    case CHAT_ERROR:       lv_label_set_text(s_status, error ? error : "出错了"); break;
    }
}

// ---- worker 回调(内部加锁;页面已删则安全返回) ----

void ui_chat_on_state(chat_state_t st, const char *error, void *user)
{
    (void)user;
    if (!bsp_lvgl_lock(500)) return;
    status_apply(st, error);
    battery_refresh();
    bsp_lvgl_unlock();
}

void ui_chat_on_message(const char *role, const char *text, void *user)
{
    (void)user;
    bool is_user = strcmp(role, "user") == 0;
    if (!bsp_lvgl_lock(500)) return;
    if (s_scroll) {
        lv_obj_t *row = add_bubble(text, is_user);
        s_msg_count++;
        if (s_msg_count > 6) {                    // 只留最近几条,控内存与遮挡
            lv_obj_t *first = lv_obj_get_child(s_scroll, 0);
            if (first) lv_obj_delete(first);
        }
        lv_obj_scroll_to_view(row, LV_ANIM_OFF);
    }
    bsp_lvgl_unlock();
}

void ui_chat_set_net(const char *text)
{
    if (!bsp_lvgl_lock(500)) return;
    if (s_net) lv_label_set_text(s_net, text);
    bsp_lvgl_unlock();
}

void ui_chat_scroll(int dir)
{
    if (!s_scroll) return;
    lv_obj_scroll_by(s_scroll, 0, dir > 0 ? -60 : 60, LV_ANIM_OFF);
}

// ---- 构建/清理 ----

void ui_chat_build(lv_obj_t *scr)
{
    s_scr = scr;
    s_msg_count = 0;

    // 电池:标题牌占左上、云朵装饰占右上(硬件指南),故放状态面板右上角
    s_scroll = lv_obj_create(scr);
    lv_obj_set_pos(s_scroll, CHAT_AREA_X, CHAT_AREA_Y);
    lv_obj_set_size(s_scroll, CHAT_AREA_W, CHAT_AREA_H);
    lv_obj_set_style_bg_color(s_scroll, lv_color_hex(UI_SKY), 0);
    lv_obj_set_style_border_color(s_scroll, lv_color_hex(UI_INK), 0);
    lv_obj_set_flex_flow(s_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *panel = ui_pixel_panel_create(scr, 12, STATUS_PANEL_Y, CHAT_AREA_W, 52, UI_PAPER);

    s_status = lv_label_create(panel);
    lv_obj_set_style_text_font(s_status, FONT_CJK, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_INK), 0);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, 150);              // 右侧留给电池
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 6, 4);

    s_batt = lv_label_create(panel);
    lv_obj_set_style_text_font(s_batt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_batt, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_batt, LV_ALIGN_TOP_RIGHT, -6, 4);
    battery_refresh();

    s_net = lv_label_create(panel);
    lv_obj_set_style_text_font(s_net, FONT_CJK, 0);
    lv_obj_set_style_text_color(s_net, lv_color_hex(0x5A6B7A), 0);
    lv_label_set_long_mode(s_net, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_net, CHAT_AREA_W - 16);
    lv_obj_align(s_net, LV_ALIGN_BOTTOM_LEFT, 6, -4);
    lv_label_set_text(s_net, "网络: 连接中...");

    status_apply(CHAT_IDLE, "");
}

void ui_chat_teardown(void)
{
    s_scr = s_scroll = s_status = s_net = s_batt = NULL;
    s_msg_count = 0;
}
