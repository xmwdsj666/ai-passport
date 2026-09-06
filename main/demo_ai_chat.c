// main/demo_ai_chat.c —— 「AI Chat」页:按住【下】键说话,云端识别后由大模型回答。
// 页面只做三件事:建界面/收按键/管生命周期;一切慢活都在 voice_pipeline worker。
//
// 按键语义(本页内):
//   DOWN 按住   说话(松开发送);DOWN 短按(非说话尾巴) 向下翻历史
//   UP   短按   向上翻历史
//   OK   短按   停止播放/中止当前轮
//   OK   长按   返回菜单(由 main.c 统一拦截,本文件不处理)
#include "demo.h"
#include "ui_chat.h"
#include "voice_pipeline.h"
#include "wifi_sta.h"

#include "bsp_display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include <stdio.h>

static const char *TAG = "demo_ai_chat";

static lv_obj_t *s_scr;
static TaskHandle_t s_net_task;
static volatile bool s_net_busy;
static volatile bool s_key_in_ppt;            // DOWN 按下正在触发说话(吞掉其 CLICK 尾巴)
static bool s_pipeline_up;

// ---------------- 网络任务(连接可能阻塞 10s,不能在 LVGL/按键上下文做) ----------------

static void net_task(void *arg)
{
    (void)arg;
    s_net_busy = true;
    esp_err_t err = wifi_sta_connect();
    char msg[80];
    if (err == ESP_OK) {
        snprintf(msg, sizeof(msg), "WiFi 已连接 %s", wifi_sta_ip());
        ui_chat_set_net(msg);
    } else {
        snprintf(msg, sizeof(msg), "WiFi 连接失败(%s),请检查配置",
                 esp_err_to_name(err));
        ui_chat_set_net(msg);
        ESP_LOGE(TAG, "WiFi 连接失败: %s", esp_err_to_name(err));
    }
    s_net_busy = false;
    vTaskDelete(NULL);                        // 一次性任务,自灭
}

// ---------------- pipeline 回调(已在 worker 任务;ui_chat_* 内部自行加锁) ----------------

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

void demo_ai_chat_enter(void)
{
    s_scr = ui_pixel_screen_create("AI CHAT");
    ui_chat_build(s_scr);
    chat_core_init();

    const pipeline_callbacks_t cbs = { on_state, on_message, NULL };
    s_pipeline_up = (voice_pipeline_start(&cbs) == ESP_OK);
    if (!s_pipeline_up) {
        ESP_LOGE(TAG, "流水线任务创建失败");
        ui_chat_set_net("内部错误: 任务创建失败");
    }

    // WiFi 后台连接,结果经 ui_chat_set_net 回显
    s_net_busy = false;
    if (xTaskCreate(net_task, "ai_net", 4096, NULL, 3, &s_net_task) != pdPASS) {
        s_net_task = NULL;
        ui_chat_set_net("内部错误: 网络任务创建失败");
    }

    lv_screen_load(s_scr);
}

void demo_ai_chat_exit(void)
{
    // 1) 先停流水线(握手等待其结束当前录音/HTTP/播放)
    if (s_pipeline_up) {
        voice_pipeline_stop();
        s_pipeline_up = false;
    }
    // 2) 等网络任务自然结束(连接尝试最多 10s),避免它引用已删的 UI
    int waited = 0;
    while (s_net_busy && waited < 12000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited += 50;
    }
    // 3) 释放无线栈,再删屏
    wifi_sta_disconnect();
    ui_chat_teardown();
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    s_key_in_ppt = false;
}

void demo_ai_chat_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (btn == BSP_BTN_DOWN && ev == BSP_BTN_PRESS) {
        // 按住说话:空闲或错误态都可发起(错误态自动清错重开)
        if (wifi_sta_is_connected() && !s_net_busy) {
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
