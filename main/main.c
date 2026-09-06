// main/main.c —— 独立应用外壳:初始化硬件 → WiFi/SNTP/天气后台服务 → 直接进主页。
// 四页路由:上/下短按切页(主页→天气→日历→对话循环);OK 长按全局回主页;
// OK 短按与对话页内按键交由当前页处理。不再使用上游 DEMOS[] 演示菜单。
//
// 按键语义(全局统一):
//   上/下 短按   切换页面
//   OK    短按   当前页自定义(对话页=停止播放;天气页=立即刷新)
//   OK    长按   返回主页(外壳统一拦截)
//   下键  按住   对话页内=说话(其余页同短按切页;对话页内 PRESS 被吃掉)
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"
#include "app_pages.h"
#include "page_home.h"
#include "page_weather.h"
#include "qweather.h"
#include "serial_screenshot.h"
#include "time_sync.h"
#include "ui_pixel.h"
#include "voice_pipeline.h"
#include "wifi_sta.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

static const char *TAG = "app";

static const app_page_t *const kPages[PAGE_COUNT] = {
    &page_home_impl, &page_weather_impl, &page_calendar_impl, &page_chat_impl,
};

static int s_current = PAGE_HOME;
static bool s_switching;                    // 切页进行中(防止按键重入)
static TaskHandle_t s_net_task;
static volatile bool s_net_busy;

static void enter_page(page_id_t id);
static void leave_current(void);

// ---- 按键路由(回调在 button 组件任务;LVGL 操作必须持锁) ----

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (s_switching) return;

    // 对话页按键处理先行:按住下键说话,CLICK 尾巴由页面吞掉
    if (s_current == PAGE_CHAT) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            // 回主页
            if (!bsp_lvgl_lock(500)) return;
            leave_current();
            s_current = PAGE_HOME;
            enter_page(PAGE_HOME);
            bsp_lvgl_unlock();
            return;
        }
        if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) {
            // 对话页内:上键=切页(本页无滚动冲突);下键 CLICK 交页面(吞说话尾巴)
            if (!bsp_lvgl_lock(500)) return;
            leave_current();
            s_current = (s_current + 1) % PAGE_COUNT;
            enter_page(s_current);
            bsp_lvgl_unlock();
            return;
        }
        kPages[s_current]->key(btn, ev);
        return;
    }

    if (!bsp_lvgl_lock(500)) return;
    if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
        if (s_current != PAGE_HOME) {
            leave_current();
            s_current = PAGE_HOME;
            enter_page(PAGE_HOME);
        }
    } else if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP) {
            leave_current();
            s_current = (s_current + PAGE_COUNT - 1) % PAGE_COUNT;
            enter_page(s_current);
        } else if (btn == BSP_BTN_DOWN) {
            leave_current();
            s_current = (s_current + 1) % PAGE_COUNT;
            enter_page(s_current);
        } else if (btn == BSP_BTN_OK) {
            kPages[s_current]->key(btn, ev);   // OK 短按交页面(天气刷新等)
        }
    }
    bsp_lvgl_unlock();
}

static void leave_current(void)
{
    // 离开对话页前先发中止:录音/播放立刻停,HTTP 等待最多一个响应周期
    if (s_current == PAGE_CHAT) {
        voice_pipeline_abort();
    }
    kPages[s_current]->exit();
}

static void enter_page(page_id_t id)
{
    kPages[id]->enter();
}

// ---- 网络/时间/天气 启动任务(连接阻塞 10s,不能占 LVGL/按键上下文) ----

static void net_boot_task(void *arg)
{
    (void)arg;
    s_net_busy = true;
    esp_err_t err = ai_wifi_sta_connect();
    if (err == ESP_OK) {
        time_sync_start();
        qweather_start();
    } else {
        ESP_LOGE(TAG, "WiFi 连接失败: %s(天气/对时不可用,稍后按重试路径)",
                 esp_err_to_name(err));
    }
    s_net_busy = false;
    vTaskDelete(NULL);
}

// 天气快照更新 → 通知主页/天气页重绘(worker 上下文;两页内部仅置脏标记)
static void on_weather_update(void *user)
{
    (void)user;
    page_home_notify_data();
    page_weather_notify_data();
}

void app_main(void)
{
    ESP_LOGI(TAG, "口袋话友 独立应用启动");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();

    // 显示是硬依赖:失败则无 UI 可言
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示初始化失败。检查 SPI(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 软依赖:单项失败不阻塞(对话页内会给出错误提示)
    bsp_button_init(on_key, NULL);
    bsp_audio_init();
    bsp_battery_init();

    // WiFi 后台连接 → SNTP/天气;期间主页显示"等待网络对时"
    qweather_set_update_cb(on_weather_update, NULL);
    if (xTaskCreate(net_boot_task, "net_boot", 6144, NULL, 3, &s_net_task) != pdPASS) {
        ESP_LOGE(TAG, "网络启动任务创建失败");
    }

    // 进入主页
    if (bsp_lvgl_lock(1000)) {
        enter_page(PAGE_HOME);
        bsp_lvgl_unlock();
    }

    serial_screenshot_start();            // FAP_SCREENSHOT_V1(发布/调试抓屏,纯观测)
    ESP_LOGI(TAG, "就绪");
}
