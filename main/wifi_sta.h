// main/wifi_sta.h —— 应用层 WiFi STA 连接管理。
// 遵循仓库约定:无线栈随页面进出启停(enter 时 connect,exit 时 disconnect);
// NVS/netif/事件循环的一次性准备复用 demo_radio。凭证来自 Kconfig,不写入代码仓库。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

// 连接并等待拿到 IP。幂等:已连接直接返回 OK。
// 失败返回错误码,调用方据状态栏提示,不阻塞页面按键。
esp_err_t ai_wifi_sta_connect(void);

// 断开并停掉/释放无线栈(页面退出时调用)。幂等。
esp_err_t ai_wifi_sta_disconnect(void);

bool ai_wifi_sta_is_connected(void);

// 已连接时返回点分 IP 字符串(内部静态缓冲);未连接返回 NULL。
const char *ai_wifi_sta_ip(void);
