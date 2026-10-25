idf_component_register(
    SRCS "main.c" "app_config.c" "leds.c" "net.c" "console.c" "tailscale.c" "status_led.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event mqtt json driver esp_driver_uart esp_driver_ledc esp_driver_rmt esp_driver_usb_serial_jtag microlink
)
