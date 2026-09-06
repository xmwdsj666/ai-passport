// main/time_sync.c —— SNTP 对时:默认阿里云 NTP,可 Kconfig 覆盖;就绪时回调通知。
// 只用 esp_netif_sntp 公共 API,不与 lwip 旧版 esp_sntp.h 混用。
#include "time_sync.h"

#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "esp_sntp.h"          // esp_sntp_set_sync_interval / esp_sntp_init(经 esp_netif_sntp 传递)
#include "sdkconfig.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "time_sync";

static volatile bool s_synced;
static int64_t s_sync_epoch;
static void (*s_on_sync)(void *user);
static void *s_on_sync_user;
static bool s_started;

static void sntp_cb(struct timeval *tv)
{
    s_synced = true;
    s_sync_epoch = (int64_t)tv->tv_sec;
    ESP_LOGI(TAG, "时间已同步: %lld", (long long)s_sync_epoch);
    if (s_on_sync) s_on_sync(s_on_sync_user);
}

esp_err_t time_sync_start(void)
{
    if (s_started) return ESP_OK;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_TIME_SYNC_NTP_SERVER);
    cfg.sync_cb = sntp_cb;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SNTP 已初始化,复用");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "sntp init 失败: %s", esp_err_to_name(err));
        return err;
    }
    // 时区:POSIX 串西负东正取负号,UTC+8 → "UTC-8"
    char tz[16];
    snprintf(tz, sizeof(tz), "UTC-%d", CONFIG_TIME_SYNC_TZ_OFFSET);
    setenv("TZ", tz, 1);
    tzset();
    esp_sntp_set_sync_interval(6 * 60 * 60 * 1000);   // 6 小时再校一次
    esp_sntp_init();
    s_started = true;
    ESP_LOGI(TAG, "SNTP 启动: %s (TZ=%s)", CONFIG_TIME_SYNC_NTP_SERVER, tz);
    return ESP_OK;
}

bool time_sync_ready(void)
{
    return s_synced;
}

int64_t time_sync_epoch(void)
{
    return s_sync_epoch;
}

void time_sync_set_cb(void (*cb)(void *user), void *user)
{
    s_on_sync = cb;
    s_on_sync_user = user;
}
