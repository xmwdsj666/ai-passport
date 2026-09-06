// main/ui_chat.h —— AI 对话页 LVGL 界面:对话气泡、状态栏、网络提示、电池。
// 所有函数假定已持有 LVGL 上下文:ui_chat_on_* 由 pipeline worker 调用,
// 内部自行 bsp_lvgl_lock();构建/滚动由页面在 LVGL 任务内调用。
#pragma once

#include "chat_core.h"
#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 在页面屏上构建布局(气泡滚动区/状态栏/网络行/电池)。重复调用前需先 teardown。
void ui_chat_build(lv_obj_t *scr);

// 页面退出删屏后调用:清空内部对象指针,保证迟到的 worker 回调安全返回。
void ui_chat_teardown(void);

// worker 回调:状态变化(内部加锁,页面已删时安全)。
void ui_chat_on_state(chat_state_t st, const char *error, void *user);

// worker 回调:新消息(已入历史;内部加锁,页面已删时安全)。
void ui_chat_on_message(const char *role, const char *text, void *user);

// 网络任务回报(内部加锁):text 如 "WiFi 已连接 192.168.1.5" / "WiFi 失败: ..."。
void ui_chat_set_net(const char *text);

// 历史滚动(dir>0 向下翻,dir<0 向上翻),由页面按键在 LVGL 上下文调用。
void ui_chat_scroll(int dir);

#ifdef __cplusplus
}
#endif
