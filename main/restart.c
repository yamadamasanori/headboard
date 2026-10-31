#include "restart.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "usage.h"

static const char *TAG = "restart";

void restart_now(void)
{
    usage_flush();
    esp_restart();
}

static void restart_cb(void *arg)
{
    restart_now();
}

void restart_later(uint32_t delay_ms)
{
    const esp_timer_create_args_t ta = {.callback = restart_cb, .name = "restart"};
    esp_timer_handle_t t;
    if (esp_timer_create(&ta, &t) != ESP_OK || esp_timer_start_once(t, (uint64_t)delay_ms * 1000) != ESP_OK) {
        ESP_LOGW(TAG, "cannot schedule restart, restarting now");
        restart_now();
    }
}
