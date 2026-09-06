idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "chat_core.c"
         "font_ai_chat_16.c"
         "wifi_sta.c"
         "ai_client.c"
         "voice_pipeline.c"
         "ui_chat.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_wifi.c"
         "demo_ble.c"
         "demo_low_power.c"
         "demo_ai_chat.c"
         "serial_screenshot.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_system esp_timer esp_wifi nvs_flash
             esp_http_client esp-tls esp_driver_usb_serial_jtag
)
