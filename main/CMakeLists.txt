set(srcs "main.c" "app_config.c" "leds.c" "net.c" "console.c" "commands.c" "tailscale.c" "status.c"
         "provision.c" "webui.c" "logbuf.c" "ota.c" "button.c" "usage.c" "ha.c" "broker.c" "restart.c")
# 状態表示: SSD1306 の OLED はどのボードでもつなげる（XIAO は標準、N16R8 は設定したときだけ）。
# 基板上のフルカラー LED は N16R8 だけ
list(APPEND srcs "status_oled.c" "oled_font.c")
if(NOT CONFIG_HEADBOARD_BOARD_XIAO)
    list(APPEND srcs "status_led.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event mqtt json driver esp_driver_uart esp_driver_ledc esp_driver_rmt
             esp_driver_i2c esp_driver_usb_serial_jtag esp_driver_gpio microlink wifi_provisioning esp_http_server
             app_update esp_timer efuse mbedtls mdns bootloader_support esp_driver_tsens esp_http_client
             esp_https_ota espcoredump spi_flash
    # 本体の設定ページとライセンス表示を組み込む
    EMBED_FILES "../web/index.html" "../LICENSE" "../THIRD_PARTY_NOTICES.md"
)

# sdkconfig のボードと、プロジェクト名（CMakeLists.txt の HB_BOARD で決まる）が合っているか確かめる
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(hb_project PROJECT_NAME)
    if(CONFIG_HEADBOARD_BOARD_XIAO AND NOT hb_project STREQUAL "headboard_led_xiao")
        message(FATAL_ERROR "sdkconfig は XIAO 用ですが、-DHB_BOARD=xiao が付いていません")
    elseif(NOT CONFIG_HEADBOARD_BOARD_XIAO AND hb_project STREQUAL "headboard_led_xiao")
        message(FATAL_ERROR "-DHB_BOARD=xiao ですが、sdkconfig が XIAO 用ではありません（sdkconfig.xiao を消してやり直してください）")
    endif()
endif()
