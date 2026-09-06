// main/app_pages.h —— 独立应用的页面定义:四页(主页/天气/日历/AI 对话)与统一接口。
// 页面接口沿用上游 demo 约定:enter 建屏并载入,exit 删屏停资源,key 收页面级按键
// (OK 长按已被外壳拦截为"回主页")。
#pragma once

#include "bsp_button.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PAGE_HOME = 0,     // 时钟 + 日期农历 + 天气摘要
    PAGE_WEATHER,      // 实时天气 + 三日预报
    PAGE_CALENDAR,     // 当月日历(农历/节日/休班角标)
    PAGE_CHAT,         // AI 语音对话
    PAGE_COUNT,
} page_id_t;

typedef struct {
    const char *name;                            // 屏顶标题(ASCII,Montserrat)
    void (*enter)(void);                         // 建屏并载入
    void (*exit)(void);                          // 删屏、停定时器、释放资源
    void (*key)(bsp_btn_t btn, bsp_btn_ev_t ev); // 页面级按键
} app_page_t;

// 各页实现(page_*.c)
extern const app_page_t page_home_impl;
extern const app_page_t page_weather_impl;
extern const app_page_t page_calendar_impl;
extern const app_page_t page_chat_impl;

// 供各页复用:电池标签文本(休 "--",否则百分比)。
typedef struct _lv_obj_t lv_obj_t;   // 前置声明(完整定义在 lvgl.h)
void page_util_battery_text(lv_obj_t *label);

#ifdef __cplusplus
}
#endif
