// main/qweather.c —— 和风天气设备端实现。
// 链路:esp_http_client 拉 gzip 响应 → 手剥 gzip 头 + miniz 裸 deflate 解压 →
// qweather_parse 纯逻辑解析 → 快照加锁更新 + NVS 落盘(断网冷启动显示最近数据)。
// worker 自有任务,页面通过快照拷贝读取,永不阻塞 UI。
#include "qweather.h"

#include "lunar.h"
#include "qweather_parse.h"
#include "sdkconfig.h"
#include "wifi_sta.h"

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "miniz.h"
#include "nvs.h"
#include <string.h>
#include <time.h>

static const char *TAG = "qweather";

#define HTTP_TIMEOUT_MS  10000
#define GZ_BUF_CAP       2048      // gzip 响应体上限(实测 ~300B)
#define JSON_BUF_CAP     8192      // 解压后 JSON 上限(实测 ~1.7KB)
#define NVS_NAMESPACE    "qweather"
#define NVS_KEY_SNAP     "snap"

static qw_snapshot_t s_snap;               // 受 s_lock 保护的最新快照
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile bool s_quit;
static volatile bool s_refresh_req;        // 立即刷新请求
static char s_city[QW_TEXT_MAX];

// UI 刷新回调(快照变化时,worker 上下文;UI 侧自行持 LVGL 锁)
static void (*s_on_update)(void *user);
static void *s_on_update_user;

static const char *city_label(void) { return CONFIG_QWEATHER_LOCATION_NAME; }

// ---- gzip → 明文 JSON ----

// 和风响应为 gzip(1f 8b)。跳过可选头字段后按裸 deflate 解压(miniz)。
static int gunzip_to_json(const uint8_t *gz, size_t gz_len, char *out, size_t cap)
{
    if (gz_len < 18 || gz[0] != 0x1F || gz[1] != 0x8B) return -1;
    uint8_t flags = gz[3];
    size_t off = 10;                        // 固定头 10 字节
    if (flags & 0x04) {                     // FEXTRA
        if (off + 2 > gz_len) return -1;
        uint16_t extra = (uint16_t)(gz[off] | (gz[off + 1] << 8));
        off += 2 + extra;
    }
    if (flags & 0x08) {                     // FNAME
        while (off < gz_len && gz[off] != 0) off++;
        off++;
    }
    if (flags & 0x10) {                     // FCOMMENT
        while (off < gz_len && gz[off] != 0) off++;
        off++;
    }
    if (flags & 0x02) off += 2;             // FHCRC
    if (off >= gz_len) return -1;

    mz_ulong out_len = cap - 1;
    (void)out_len;
    // gzip = 裸 deflate(无 zlib 头),flags 传 0
    int rc = (int)tinfl_decompress_mem_to_mem(out, cap - 1, gz + off, gz_len - off, 0);
    if (rc == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
        ESP_LOGE(TAG, "gzip 解压失败(输入 %u 字节)", (unsigned)(gz_len - off));
        return -1;
    }
    out[rc] = '\0';
    return rc;
}

// ---- HTTP 拉取 ----

// 成功返回解压后 JSON 长度;失败 -1。
static int fetch_endpoint(const char *path, char *json, size_t json_cap)
{
    char url[256];
    int n = snprintf(url, sizeof(url), "https://%s%s?location=%s",
                     CONFIG_QWEATHER_HOST, path, CONFIG_QWEATHER_LOCATION_ID);
    if (n < 0 || (size_t)n >= sizeof(url)) return -1;

    uint8_t *gz = malloc(GZ_BUF_CAP);
    if (!gz) return -1;
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(gz);
        return -1;
    }
    esp_http_client_set_header(client, "X-QW-Api-Key", CONFIG_QWEATHER_API_KEY);
    esp_http_client_set_header(client, "Accept-Encoding", "gzip");

    int out_len = -1;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        size_t used = 0;
        while (used < GZ_BUF_CAP - 1) {
            int r = esp_http_client_read(client, (char *)gz + used,
                                         (int)(GZ_BUF_CAP - 1 - used));
            if (r <= 0) break;
            used += (size_t)r;
        }
        gz[used] = '\0';
        if (status == 200 && used > 0) {
            char *json_tmp = malloc(JSON_BUF_CAP);
            if (json_tmp) {
                int jlen = gunzip_to_json(gz, used, json_tmp, JSON_BUF_CAP);
                if (jlen > 0 && (size_t)jlen < json_cap) {
                    memcpy(json, json_tmp, (size_t)jlen + 1);
                    out_len = jlen;
                }
                free(json_tmp);
            }
        } else {
            // 错误响应也解出来打日志,便于排查(Key/Host/限额)
            char *json_tmp = malloc(JSON_BUF_CAP);
            if (json_tmp) {
                int jlen = gunzip_to_json(gz, used, json_tmp, JSON_BUF_CAP);
                if (jlen > 0) ESP_LOGE(TAG, "HTTP %d: %.200s", status, json_tmp);
                else ESP_LOGE(TAG, "HTTP %d, body %u 字节不可解压", status, (unsigned)used);
                free(json_tmp);
            }
        }
    } else {
        ESP_LOGE(TAG, "连接 %s 失败", CONFIG_QWEATHER_HOST);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(gz);
    return out_len;
}

// ---- NVS 缓存 ----

static void cache_save(const qw_snapshot_t *snap)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_SNAP, snap, sizeof(*snap));
    nvs_commit(h);
    nvs_close(h);
}

static void cache_load(qw_snapshot_t *snap)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(*snap);
    if (nvs_get_blob(h, NVS_KEY_SNAP, snap, &len) != ESP_OK || len != sizeof(*snap)) {
        memset(snap, 0, sizeof(*snap));
    }
    nvs_close(h);
}

// ---- 刷新一轮 ----

static esp_err_t refresh_once(void)
{
    if (CONFIG_QWEATHER_API_KEY[0] == '\0') {
        ESP_LOGW(TAG, "未配置 QWEATHER_API_KEY,跳过刷新");
        return ESP_ERR_INVALID_STATE;
    }
    char *json = malloc(JSON_BUF_CAP);
    if (!json) return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_FAIL;

    qw_now_t now;
    qw_daily_t daily[QW_DAILY_MAX];
    int daily_count = 0;
    bool now_ok = false, daily_ok = false;

    if (fetch_endpoint("/v7/weather/now", json, JSON_BUF_CAP) > 0) {
        char ec[8];
        now_ok = qw_parse_now(json, &now, ec, sizeof(ec));
        if (!now_ok) ESP_LOGE(TAG, "now 解析失败 code=%s", ec);
    }
    if (fetch_endpoint("/v7/weather/3d", json, JSON_BUF_CAP) > 0) {
        char ec[8];
        daily_count = qw_parse_daily(json, daily, QW_DAILY_MAX, ec, sizeof(ec));
        daily_ok = daily_count > 0;
        if (!daily_ok) ESP_LOGE(TAG, "3d 解析失败 code=%s", ec);
    }
    if (now_ok || daily_ok) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
            time_t now_t = time(NULL);
            if (now_ok) {
                s_snap.now = now;
                s_snap.fetched_at_sec = (int64_t)now_t;
            }
            if (daily_ok) {
                memcpy(s_snap.daily, daily, sizeof(qw_daily_t) * (size_t)daily_count);
                s_snap.daily_count = daily_count;
            }
            qw_snapshot_t copy = s_snap;
            xSemaphoreGive(s_lock);
            cache_save(&copy);
            if (s_on_update) s_on_update(s_on_update_user);
            err = ESP_OK;
        }
    }
    free(json);
    return err;
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_quit) break;
        if (ai_wifi_sta_is_connected() && CONFIG_QWEATHER_API_KEY[0] != '\0') {
            refresh_once();
            // 刷新完睡到下一周期或提前唤醒
            int waited = 0;
            int period = CONFIG_QWEATHER_REFRESH_MIN * 60 * 1000;
            while (waited < period && !s_refresh_req && !s_quit) {
                vTaskDelay(pdMS_TO_TICKS(500));
                waited += 500;
            }
            s_refresh_req = false;
        } else {
            vTaskDelay(pdMS_TO_TICKS(2000));   // 无网/无 Key:慢轮询等待
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

// ---- 对外接口 ----

esp_err_t qweather_start(void)
{
    if (s_lock) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    memset(&s_snap, 0, sizeof(s_snap));
    cache_load(&s_snap);
    snprintf(s_city, sizeof(s_city), "%s", CONFIG_QWEATHER_LOCATION_NAME);
    if (xTaskCreate(worker, "qweather", 6144, NULL, 3, &s_task) != pdPASS) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void qweather_stop(void)
{
    if (!s_lock) return;
    s_quit = true;
    int waited = 0;
    while (s_task && waited < 5000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited += 50;
    }
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    s_quit = false;
}

void qweather_refresh_now(void)
{
    s_refresh_req = true;
}

void qweather_get_snapshot(qw_snapshot_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        *out = s_snap;
        xSemaphoreGive(s_lock);
    }
}

void qweather_set_update_cb(void (*cb)(void *user), void *user)
{
    s_on_update = cb;
    s_on_update_user = user;
}
