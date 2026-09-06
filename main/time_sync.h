// main/time_sync.h —— SNTP 网络对时。WiFi 连接后启动;就绪回调通知 UI 刷新时钟。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 幂等启动(内部只装一次 SNTP;重复调用无害)。时区取 Kconfig(默认 UTC+8)。
esp_err_t time_sync_start(void);

// 系统时间是否已从 NTP 同步过(同步后即便短暂断网仍为 true)。
bool time_sync_ready(void);

// 最近同步时间(epoch 秒);未同步返回 0。
int64_t time_sync_epoch(void);

// 注册同步完成回调(仅在 start 前有效)。
void time_sync_set_cb(void (*cb)(void *user), void *user);

#ifdef __cplusplus
}
#endif
