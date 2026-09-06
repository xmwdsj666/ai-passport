// main/wifi_sta.c —— STA 连接实现:同步等待 GOT_IP,失败自动重试一次。
// 栈生命周期与页面一致:connect 时建,disconnect 时完整释放(参照 demo_wifi.c 的停机顺序)。
#include "wifi_sta.h"

#include "demo_radio.h"
#include "sdkconfig.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "wifi_sta";

#define CONNECT_TIMEOUT_MS 10000
#define CONNECT_MAX_RETRY  1

typedef enum {
    WIFI_STA_EVT_GOT_IP = 0,
    WIFI_STA_EVT_FAIL,
} wifi_sta_evt_t;

static esp_netif_t *s_netif;
static esp_event_handler_instance_t s_wifi_evt;
static esp_event_handler_instance_t s_ip_evt;
static bool s_wifi_inited;
static bool s_wifi_started;
static SemaphoreHandle_t s_sem;                 // 惰性创建,应用生命周期持有
static volatile wifi_sta_evt_t s_result;
static volatile int s_retry;
static volatile bool s_connected;
static char s_ip_str[16];

static void emit(wifi_sta_evt_t evt)
{
    s_result = evt;
    if (s_sem) xSemaphoreGive(s_sem);
}

// 事件回调运行在 esp_event 任务,只做轻量置位。
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < CONNECT_MAX_RETRY) {       // 自动重试一次(应付路由器慢响应)
            s_retry++;
            esp_wifi_connect();
            return;
        }
        s_connected = false;
        emit(WIFI_STA_EVT_FAIL);
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&evt->ip_info.ip));
        s_connected = true;
        emit(WIFI_STA_EVT_GOT_IP);
    }
}

bool wifi_sta_is_connected(void)
{
    return s_connected;
}

const char *wifi_sta_ip(void)
{
    return s_connected ? s_ip_str : NULL;
}

static void stack_destroy(void)
{
    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_inited) {
        esp_wifi_deinit();
        s_wifi_inited = false;
    }
    if (s_netif) {
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
    }
    s_connected = false;
    s_ip_str[0] = '\0';
}

esp_err_t wifi_sta_connect(void)
{
    if (s_connected) return ESP_OK;
    if (s_ip_str[0] == '\0' && !s_sem) {
        s_sem = xSemaphoreCreateBinary();
        if (!s_sem) return ESP_ERR_NO_MEM;
    }

    esp_err_t err = demo_radio_nvs_prepare();
    if (err != ESP_OK) return err;
    err = demo_radio_network_prepare();
    if (err != ESP_OK) return err;

    if (!s_netif) {
        s_netif = esp_netif_create_default_wifi_sta();
        if (!s_netif) return ESP_ERR_NO_MEM;
    }
    if (!s_wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) goto fail;
        s_wifi_inited = true;
        err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                  on_wifi_event, NULL, &s_wifi_evt);
        if (err != ESP_OK) goto fail;
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                  on_ip_event, NULL, &s_ip_evt);
        if (err != ESP_OK) goto fail;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) goto fail;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) goto fail;

    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, CONFIG_AI_CHAT_WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, CONFIG_AI_CHAT_WIFI_PASSWORD, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) goto fail;

    err = esp_wifi_start();
    if (err != ESP_OK) goto fail;
    s_wifi_started = true;

    // 空凭证直接报配置错误,不做无谓扫描。
    if (CONFIG_AI_CHAT_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "未配置 WiFi SSID(menuconfig: AI_CHAT_WIFI_SSID)");
        err = ESP_ERR_INVALID_STATE;
        goto fail_started;
    }

    s_retry = 0;
    ESP_LOGI(TAG, "连接 \"%s\" ...", CONFIG_AI_CHAT_WIFI_SSID);
    err = esp_wifi_connect();
    if (err != ESP_OK) goto fail_started;

    // 同步等结果;超时按失败处理(事件里可能还会触发重试,忽略其迟到结果)。
    if (xSemaphoreTake(s_sem, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS)) != pdTRUE ||
        s_result != WIFI_STA_EVT_GOT_IP) {
        ESP_LOGE(TAG, "连接失败/超时");
        err = ESP_ERR_TIMEOUT;
        goto fail_started;
    }
    ESP_LOGI(TAG, "已连接,IP=%s", s_ip_str);
    return ESP_OK;

fail_started:
    stack_destroy();
    return err;

fail:
    stack_destroy();
    return err;
}

esp_err_t wifi_sta_disconnect(void)
{
    if (!s_wifi_inited && !s_netif) return ESP_OK;
    stack_destroy();
    ESP_LOGI(TAG, "无线栈已释放");
    return ESP_OK;
}
