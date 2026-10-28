idf_component_register(
    SRCS "main.c" "app_config.c" "leds.c" "net.c" "console.c" "commands.c" "tailscale.c" "status_led.c"
         "provision.c" "webui.c" "logbuf.c" "ota.c" "button.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event mqtt json driver esp_driver_uart esp_driver_ledc esp_driver_rmt
             esp_driver_usb_serial_jtag esp_driver_gpio microlink wifi_provisioning esp_http_server app_update
             esp_timer efuse mbedtls mdns bootloader_support
    # 本体の設定ページとライセンス表示を組み込む
    EMBED_FILES "../web/index.html" "../LICENSE" "../THIRD_PARTY_NOTICES.md"
)
