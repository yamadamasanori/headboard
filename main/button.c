#include "button.h"

#include "app_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usage.h"
#include "status_led.h"
#include "tailscale.h"

static const char *TAG = "button";

#define BUTTON_GPIO      GPIO_NUM_0
#define POLL_MS          50
#define WIFI_RESET_MS    3000
#define FACTORY_RESET_MS 10000

static void button_task(void *arg)
{
    int held_ms = 0;
    bool armed = false;  // 一度離れた（High になった）のを見てから数える。押したまま起動したときや配線の不具合で誤動作しないように
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        int level = gpio_get_level(BUTTON_GPIO);
        if (!armed) {
            armed = level == 1;
            continue;
        }
        if (level == 0) {  // 押すと Low
            held_ms += POLL_MS;
            if (held_ms == WIFI_RESET_MS) {
                ESP_LOGI(TAG, "release now to clear Wi-Fi settings (keep holding for full reset)");
                status_led_set_hint(STATUS_HINT_WIFI_RESET);
            } else if (held_ms == FACTORY_RESET_MS) {
                ESP_LOGI(TAG, "release now to clear all settings");
                status_led_set_hint(STATUS_HINT_FACTORY_RESET);
            }
            continue;
        }
        if (held_ms >= FACTORY_RESET_MS) {
            ESP_LOGW(TAG, "factory reset by button");
            config_factory_reset();
            ts_factory_reset();
            esp_restart();
        } else if (held_ms >= WIFI_RESET_MS) {
            ESP_LOGW(TAG, "Wi-Fi settings cleared by button");
            config_clear_wifi();
            usage_flush();
            esp_restart();
        }
        held_ms = 0;
    }
}

void button_start(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    xTaskCreate(button_task, "button", 3072, NULL, 2, NULL);
}
