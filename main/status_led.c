#include "status_led.h"

#include <stdbool.h>
#include <string.h>
#include "app_config.h"
#include "driver/rmt_tx.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "tailscale.h"

static const char *TAG = "status_led";

#define RMT_RES_HZ     10000000  // 1 tick = 0.1 us
#define TICK_MS        20
#define OK_HOLD_MS     10000     // 正常になってから消灯するまで
#define PULSE_MS       150

typedef struct {
    uint8_t r, g, b;
} rgb_t;

typedef enum {
    PAT_OFF,
    PAT_SOLID,
    PAT_BLINK_SLOW,   // 1 秒周期
    PAT_BLINK,        // 0.5 秒周期
    PAT_BLINK_FAST,   // 0.2 秒周期
} pattern_t;

static const rgb_t C_PURPLE = {160, 0, 255};
static const rgb_t C_BLUE = {0, 0, 255};
static const rgb_t C_CYAN = {0, 200, 255};
static const rgb_t C_YELLOW = {255, 160, 0};
static const rgb_t C_RED = {255, 0, 0};
static const rgb_t C_GREEN = {0, 255, 0};
static const rgb_t C_WHITE = {255, 255, 255};
static const rgb_t C_BLACK = {0, 0, 0};

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static volatile int64_t s_pulse_until_us;
static volatile status_hint_t s_hint;
static const char *volatile s_indicator = "starting";

static volatile bool s_busy;

static bool IRAM_ATTR on_tx_done(rmt_channel_handle_t ch, const rmt_tx_done_event_data_t *ev, void *ctx)
{
    s_busy = false;
    return false;
}

// 送信できたら true。前の送信が終わっていなければ今回は送らない（タスクを止めないため）
static bool show(rgb_t c)
{
    if (s_busy) {
        return false;
    }
    // 明るさ設定を掛け、WS2812 の並び（G, R, B）で送る
    uint8_t buf[3] = {
        (uint8_t)(c.g * g_cfg.sled_brightness / 255),
        (uint8_t)(c.r * g_cfg.sled_brightness / 255),
        (uint8_t)(c.b * g_cfg.sled_brightness / 255),
    };
    rmt_transmit_config_t tx = {.loop_count = 0};
    s_busy = true;
    if (rmt_transmit(s_chan, s_enc, buf, sizeof(buf), &tx) != ESP_OK) {
        s_busy = false;
        return false;
    }
    return true;
}

static bool blink_on(pattern_t p, int64_t now_ms)
{
    switch (p) {
    case PAT_SOLID: return true;
    case PAT_BLINK_SLOW: return (now_ms % 1000) < 500;
    case PAT_BLINK: return (now_ms % 500) < 250;
    case PAT_BLINK_FAST: return (now_ms % 200) < 100;
    default: return false;
    }
}

// 今の状態から、表示する色と光り方を決める。正常なら true
static bool decide(rgb_t *color, pattern_t *pat)
{
    net_state_t st;
    net_get_state(&st);

    if (s_hint == STATUS_HINT_WIFI_RESET) {
        *color = C_WHITE, *pat = PAT_SOLID, s_indicator = "button_wifi_reset";
    } else if (s_hint == STATUS_HINT_FACTORY_RESET) {
        *color = C_RED, *pat = PAT_SOLID, s_indicator = "button_factory_reset";
    } else if (!st.wifi_configured) {
        *color = C_PURPLE, *pat = PAT_BLINK_SLOW, s_indicator = st.ble_setup ? "ble_setup" : "wifi_unconfigured";
    } else if (!st.wifi_up) {
        *color = C_BLUE, *pat = PAT_BLINK_FAST, s_indicator = "wifi_connecting";
    } else if (g_cfg.ts_enabled && ts_has_error()) {
        *color = C_RED, *pat = PAT_BLINK, s_indicator = "tailscale_auth_error";
    } else if (g_cfg.ts_enabled && !ts_is_connected()) {
        *color = C_CYAN, *pat = PAT_BLINK, s_indicator = "tailscale_connecting";
    } else if (st.mqtt_refused) {
        *color = C_RED, *pat = PAT_BLINK, s_indicator = "mqtt_refused";
    } else if (!st.mqtt_configured || !st.mqtt_up) {
        *color = C_YELLOW, *pat = PAT_BLINK, s_indicator = "mqtt_connecting";
    } else {
        *color = C_GREEN, *pat = PAT_SOLID, s_indicator = "ok";
        return true;
    }
    return false;
}

const char *status_led_indicator(void)
{
    return g_cfg.sled_pin == STATUS_LED_DISABLED ? "disabled" : s_indicator;
}

static void led_task(void *arg)
{
    // 起動時に白を一瞬点灯（LED の点灯確認）
    show(C_WHITE);
    vTaskDelay(pdMS_TO_TICKS(300));

    int64_t ok_since_ms = -1;
    rgb_t last = {1, 2, 3};
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(TICK_MS));
        int64_t now_us = esp_timer_get_time();
        int64_t now_ms = now_us / 1000;

        rgb_t color;
        pattern_t pat;
        if (decide(&color, &pat)) {
            if (ok_since_ms < 0) {
                ok_since_ms = now_ms;
            }
            if (!g_cfg.sled_always_on && now_ms - ok_since_ms >= OK_HOLD_MS) {
                pat = PAT_OFF;
            }
        } else {
            ok_since_ms = -1;
        }

        rgb_t out = blink_on(pat, now_ms) ? color : C_BLACK;
        if (now_us < s_pulse_until_us) {
            out = C_GREEN;
        }
        if (memcmp(&out, &last, sizeof(out)) != 0 && show(out)) {
            last = out;
        }
    }
}

void status_led_set_hint(status_hint_t hint)
{
    s_hint = hint;
}

void status_led_pulse(void)
{
    s_pulse_until_us = esp_timer_get_time() + PULSE_MS * 1000;
}

void status_led_start(void)
{
    if (g_cfg.sled_pin == STATUS_LED_DISABLED) {
        ESP_LOGI(TAG, "status LED disabled");
        return;
    }
    rmt_tx_channel_config_t ch = {
        .gpio_num = g_cfg.sled_pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RES_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
    };
    if (rmt_new_tx_channel(&ch, &s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "RMT channel init failed");
        return;
    }
    // WS2812 のタイミング: 0 = 0.3us High + 0.9us Low, 1 = 0.9us High + 0.3us Low
    rmt_bytes_encoder_config_t enc = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
        .flags.msb_first = 1,
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc, &s_enc));
    rmt_tx_event_callbacks_t cbs = {.on_trans_done = on_tx_done};
    ESP_ERROR_CHECK(rmt_tx_register_event_callbacks(s_chan, &cbs, NULL));
    ESP_ERROR_CHECK(rmt_enable(s_chan));
    xTaskCreate(led_task, "status_led", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "status LED on GPIO%u", g_cfg.sled_pin);
}
