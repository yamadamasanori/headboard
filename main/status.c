#include "status.h"

#include "app_config.h"
#include "board.h"
#include "button.h"
#include "esp_timer.h"
#include "net.h"
#include "tailscale.h"

static volatile uint32_t s_held_ms;
static volatile int64_t s_pulse_at_us;
static volatile int s_pulse_light = -1;
static volatile int64_t s_wake_at_us;
static volatile int64_t s_identify_until_us;
static const char *volatile s_indicator = "starting";

status_hint_t status_hint(void)
{
    uint32_t held = s_held_ms;
    return held >= BUTTON_FACTORY_RESET_MS ? STATUS_HINT_FACTORY_RESET
           : held >= BUTTON_WIFI_RESET_MS  ? STATUS_HINT_WIFI_RESET
                                           : STATUS_HINT_NONE;
}

status_t status_decide(void)
{
    net_state_t st;
    net_get_state(&st);

    status_t s;
    const char *name;
    if (!st.wifi_configured) {
        s = st.ble_setup ? ST_BLE_SETUP : ST_WIFI_UNCONFIGURED;
        name = st.ble_setup ? "ble_setup" : "wifi_unconfigured";
    } else if (!st.wifi_up) {
        s = ST_WIFI_CONNECTING, name = "wifi_connecting";
    } else if (g_cfg.ts_enabled && ts_has_error()) {
        s = ST_TS_AUTH_ERROR, name = "tailscale_auth_error";
    } else if (g_cfg.ts_enabled && !ts_is_connected()) {
        s = ST_TS_CONNECTING, name = "tailscale_connecting";
    } else if (st.mqtt_refused) {
        s = ST_MQTT_REFUSED, name = "mqtt_refused";
    } else if (!st.mqtt_configured) {
        s = ST_MQTT_UNCONFIGURED, name = "mqtt_connecting";
    } else if (!st.mqtt_up) {
        s = ST_MQTT_CONNECTING, name = "mqtt_connecting";
    } else {
        s = ST_OK, name = "ok";
    }
    // 設定ページには、BOOT ボタンの案内を優先して出す
    switch (status_hint()) {
    case STATUS_HINT_WIFI_RESET: name = "button_wifi_reset"; break;
    case STATUS_HINT_FACTORY_RESET: name = "button_factory_reset"; break;
    default: break;
    }
    s_indicator = name;
    return s;
}

// フルカラー LED と OLED は、両方つないでいれば両方に表示する
void status_led_start(void)
{
#if !BOARD_HAS_OLED
    status_rgb_start();
#endif
    status_oled_start();
}

static bool output_enabled(void)
{
#if !BOARD_HAS_OLED
    if (status_rgb_enabled()) {
        return true;
    }
#endif
    return status_oled_enabled();
}

const char *status_led_indicator(void)
{
    return output_enabled() ? s_indicator : "disabled";
}

void status_led_button(uint32_t held_ms)
{
    s_held_ms = held_ms;
}

void status_led_wake(void)
{
    s_wake_at_us = esp_timer_get_time();
}

void status_led_pulse(int light)
{
    s_pulse_light = light;
    s_pulse_at_us = esp_timer_get_time();
}

void status_led_identify(uint32_t ms)
{
    s_identify_until_us = esp_timer_get_time() + (int64_t)ms * 1000;
}

uint32_t status_button_held_ms(void) { return s_held_ms; }
int64_t status_pulse_at_us(void) { return s_pulse_at_us; }
int status_pulse_light(void) { return s_pulse_light; }
int64_t status_wake_at_us(void) { return s_wake_at_us; }
int64_t status_identify_until_us(void) { return s_identify_until_us; }
