// main/qweather.h —— 和风天气设备端服务:HTTP 拉取(gzip 解压)+ NVS 缓存 + 定时刷新。
// 拉取在自有 worker 任务执行;页面只读缓存快照,永不阻塞。纯解析逻辑在 qweather_parse(主机可测)。
#pragma once

#include "qweather_parse.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    qw_now_t   now;
    qw_daily_t daily[QW_DAILY_MAX];
    int        daily_count;               // 0..QW_DAILY_MAX
    char       city[QW_TEXT_MAX];         // 城市名(来自 LocationID 配置注释或占位)
    int64_t    fetched_at_sec;            // 最近成功拉取的 epoch 秒;0=从无数据
} qw_snapshot_t;

// 初始化(NVS 缓存加载)并启动刷新 worker。WiFi 连接前后均可调用:
// worker 会在无网时静默等待,拿到网即拉首刷。
esp_err_t qweather_start(void);

// 停机(应用退出时;等待 worker 空闲)。
void qweather_stop(void);

// 请求立即刷新(异步;成功后快照更新)。无 Key/无网时静默失败。
void qweather_refresh_now(void);

// 读缓存快照(拷贝;任何时候可调)。fetched_at_sec==0 表示尚无数据。
void qweather_get_snapshot(qw_snapshot_t *out);

// 注册快照更新回调(worker 上下文调用;UI 侧自行持 LVGL 锁),须在 start 前设置。
void qweather_set_update_cb(void (*cb)(void *user), void *user);

#ifdef __cplusplus
}
#endif
