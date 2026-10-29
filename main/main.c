// ヘッドボード LED コントローラ
//   - 読書灯 / バックライトの 2 系統を PWM で個別に調光
//   - MQTT で Home Assistant から操作（自動探索対応）
//   - Wi-Fi は Bluetooth（スマホの公式アプリ）で設定し、その他は本体の設定ページで設定
//   - オンボード LED で接続状態を表示

#include "app_config.h"
#include "button.h"
#include "ha.h"
#include "console.h"
#include "esp_log.h"
#include "leds.h"
#include "logbuf.h"
#include "net.h"
#include "nvs_flash.h"
#include "usage.h"
#include "status_led.h"

void app_main(void)
{
    logbuf_init();  // これ以降のログを本体の設定ページでも見られるようにする

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    config_load();
    config_apply_log_levels();
    leds_init();
    usage_init();
    console_start();
    ha_init();
    net_start();
    status_led_start();
    button_start();
    ESP_LOGI("main", "headboard-led %s started (id=%s, reset: %s)", FW_VERSION, config_device_id(), logbuf_reset_reason());
}
