// main/serial_screenshot.h —— FAP_SCREENSHOT_V1 串口截图协议(发布助手抓取界面用)。
// 观测性协议:监听控制台串口,收到 "FAP_SCREENSHOT_V1\n" 后回一帧当前屏幕的
// RGB565LE 像素。不重启、不改配置、不读任何凭证;失败时静默超时。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 在 app_main(LVGL 就绪后)调用一次:创建后台监听任务。
void serial_screenshot_start(void);

#ifdef __cplusplus
}
#endif
