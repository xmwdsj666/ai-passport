// main/page_chat.c —— AI 语音对话页(page_chat_impl)。
// WiFi 由外壳开机连接,本页只管:界面构建、流水线生命周期、按键语义。
//   DOWN 按住=说话(PRESS 启动,worker 轮询松开);DOWN 短按(说话尾巴)被吞;
//   UP 短按=向上翻历史;OK 短按=停止播放/中止当前轮;OK 长按=回主页(外壳拦截)。
#include "app_pages.h"
#include "ui_chat.h"
#include "voice_pipeline.h"
#include "wifi_sta.h"

#include "bsp_display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "ui_pixel.h"
#include <stdio.h>

static const char *TAG = "page_chat";

static lv_obj_t *s_scr;
static volatile bool s_key_in_ppt;            // DOWN 按下正在触发说话(吞其 CLICK 尾巴)
static bool s_pipeline_up;

// ---------------- pipeline 回调(worker 任务;ui_chat_* 内部自行加锁) ----------------

static void on_state(chat_state_t st, const char *error, void *user)
{
    (void)user;
    ui_chat_on_state(st, error, NULL);
}

static void on_message(const char *role, const char *text, void *user)
{
    (void)user;
    ui_chat_on_message(role, text, NULL);
}

// ---------------- 页面接口 ----------------

static void page_chat_enter(void)
{
    s_scr = ui_pixel_screen_create("AI CHAT");
    ui_chat_build(s_scr);
    chat_core_init();

    const pipeline_callbacks_t cbs = { on_state, on_message, NULL };
    s_pipeline_up = (voice_pipeline_start(&cbs) == ESP_OK);
    if (!s_pipeline_up) {
        ESP_LOGE(TAG, "流水线任务创建失败");
        ui_chat_set_net("内部错误: 任务创建失败");
    } else if (!ai_wifi_sta_is_connected()) {
        ui_chat_set_net("WiFi 未连接,无法对话");
    }
    lv_screen_load(s_scr);
}

static void page_chat_exit(void)
{
    // 先停流水线(握手等待其结束当前录音/HTTP/播放)
    if (s_pipeline_up) {
        voice_pipeline_stop();
        s_pipeline_up = false;
    }
    ui_chat_teardown();
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    s_key_in_ppt = false;
}

static void page_chat_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (btn == BSP_BTN_DOWN && ev == BSP_BTN_PRESS) {
        if (ai_wifi_sta_is_connected()) {
            s_key_in_ppt = true;
            voice_pipeline_start_record();
        } else {
            ui_chat_set_net("WiFi 未连接,无法说话");
        }
        return;
    }
    if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK) {
        if (s_key_in_ppt) {
            s_key_in_ppt = false;             // 说话松开产生的 CLICK,吞掉
        } else {
            ui_chat_scroll(1);
        }
        return;
    }
    if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) {
        ui_chat_scroll(-1);
        return;
    }
    if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
        voice_pipeline_abort();               // 停播放/中止本轮;空闲时是无害空操作
        return;
    }
}

const app_page_t page_chat_impl = {"AI CHAT", page_chat_enter, page_chat_exit, page_chat_key};
