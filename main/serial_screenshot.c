// main/serial_screenshot.c —— FAP_SCREENSHOT_V1 实现。
// 控制台即 USB-Serial/JTAG:启动时尝试装驱动(控制台已装则复用),
// 收到完整命令行后,在持 LVGL 锁的前提下对当前屏幕做离屏快照,
// 回 "FAP_SCREENSHOT_V1 <w> <h> RGB565LE <len>\n" + 行主序小端像素。
#include "serial_screenshot.h"

#include "bsp_display.h"

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "draw/snapshot/lv_snapshot.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "sshot";

#define CMD_LINE_MAX   64
#define TX_CHUNK       1024
#define RESP_HEADER_MAX 80

static void send_all(const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t off = 0;
    while (off < len) {
        size_t n = len - off < TX_CHUNK ? len - off : TX_CHUNK;
        int w = usb_serial_jtag_write_bytes(p + off, n, pdMS_TO_TICKS(2000));
        if (w <= 0) {
            ESP_LOGW(TAG, "串口写中断@%u", (unsigned)off);
            return;
        }
        off += (size_t)w;
    }
}

static void respond_snapshot(void)
{
    if (!bsp_lvgl_lock(3000)) {
        ESP_LOGW(TAG, "LVGL 忙,放弃本次截图");
        return;
    }
    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    bsp_lvgl_unlock();
    if (!buf) {
        ESP_LOGE(TAG, "快照失败(内存不足?)");
        return;
    }

    uint32_t w = buf->header.w;
    uint32_t h = buf->header.h;
    uint32_t len = buf->data_size;
    char header[RESP_HEADER_MAX];
    int n = snprintf(header, sizeof(header), "FAP_SCREENSHOT_V1 %lu %lu RGB565LE %lu\n",
                     (unsigned long)w, (unsigned long)h, (unsigned long)len);
    if (n > 0 && (size_t)n < sizeof(header)) {
        ESP_LOGI(TAG, "回送截图 %lux%lu, %lu 字节", (unsigned long)w, (unsigned long)h,
                 (unsigned long)len);
        send_all(header, (size_t)n);
        send_all(buf->data, len);
    }
    lv_draw_buf_destroy(buf);
}

static void sshot_task(void *arg)
{
    (void)arg;
    char line[CMD_LINE_MAX];
    size_t used = 0;
    uint8_t rx[16];
    for (;;) {
        int n = usb_serial_jtag_read_bytes(rx, sizeof(rx), pdMS_TO_TICKS(500));
        for (int i = 0; i < n; i++) {
            char c = (char)rx[i];
            if (c == '\n') {
                // 命令行比较忽略结尾 \r 与大小写
                line[used] = '\0';
                size_t end = used;
                while (end > 0 && line[end - 1] == '\r') line[--end] = '\0';
                if (strcmp(line, "FAP_SCREENSHOT_V1") == 0) respond_snapshot();
                used = 0;
            } else if (used + 1 < sizeof(line)) {
                line[used++] = c;
            } else {
                used = 0;                     // 超长行直接丢弃
            }
        }
    }
}

void serial_screenshot_start(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    // 控制台走 USB-Serial/JTAG 时驱动可能已由系统安装:复用即可。
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "USB-Serial/JTAG 驱动已安装");
    } else if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "USB-Serial/JTAG 驱动已由控制台持有,复用");
    } else {
        ESP_LOGE(TAG, "USB-Serial/JTAG 驱动安装失败: %s,截图协议不可用",
                 esp_err_to_name(err));
        return;
    }
    if (xTaskCreate(sshot_task, "sshot", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "截图任务创建失败");
    }
}
