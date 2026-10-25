#include "tailscale.h"

#include <string.h>
#include "app_config.h"
#include "esp_log.h"
#include "microlink.h"

static const char *TAG = "tailscale";

static microlink_t *s_ml;
static ts_connected_cb_t s_on_connected;

static const char *state_name(microlink_state_t st)
{
    switch (st) {
    case ML_STATE_IDLE: return "idle";
    case ML_STATE_WIFI_WAIT: return "wifi_wait";
    case ML_STATE_CONNECTING: return "connecting";
    case ML_STATE_REGISTERING: return "registering";
    case ML_STATE_CONNECTED: return "connected";
    case ML_STATE_RECONNECTING: return "reconnecting";
    case ML_STATE_ERROR: return "error";
    }
    return "unknown";
}

static void on_state(microlink_t *ml, microlink_state_t st, void *user)
{
    ESP_LOGI(TAG, "state: %s", state_name(st));
    if (st == ML_STATE_CONNECTED) {
        char ip[16];
        microlink_ip_to_str(microlink_get_vpn_ip(ml), ip);
        ESP_LOGI(TAG, "connected to tailnet as %s (%s)", g_cfg.ts_hostname, ip);
        if (s_on_connected) {
            s_on_connected();
        }
    }
}

void ts_on_wifi_up(ts_connected_cb_t on_connected)
{
    s_on_connected = on_connected;
    if (s_ml) {
        microlink_rebind(s_ml);  // Wi-Fi が繋がり直したので、ソケットを作り直す
        return;
    }
    if (config_take_ts_reset_flag()) {
        ESP_LOGI(TAG, "auth key changed, clearing stored keys");
        microlink_factory_reset();
    }
    microlink_config_t cfg = {
        .auth_key = g_cfg.ts_authkey,
        .device_name = g_cfg.ts_hostname,
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = CONFIG_ML_MAX_PEERS,
    };
    s_ml = microlink_init(&cfg);
    if (!s_ml) {
        ESP_LOGE(TAG, "microlink_init failed (out of memory?)");
        return;
    }
    microlink_set_state_callback(s_ml, on_state, NULL);
    if (microlink_start(s_ml) != ESP_OK) {
        ESP_LOGE(TAG, "microlink_start failed");
    }
}

bool ts_is_connected(void)
{
    return s_ml && microlink_is_connected(s_ml);
}

bool ts_has_error(void)
{
    return s_ml && (microlink_auth_failed(s_ml) || microlink_get_state(s_ml) == ML_STATE_ERROR);
}

bool ts_resolve(const char *hostname, char *ip_out, size_t len)
{
    if (!ts_is_connected() || len < 16) {
        return false;
    }
    uint32_t ip = microlink_resolve(s_ml, hostname);
    if (!ip) {
        return false;
    }
    microlink_ip_to_str(ip, ip_out);
    return true;
}

void ts_factory_reset(void)
{
    microlink_factory_reset();
}

void ts_status_to_json(cJSON *o)
{
    cJSON *t = cJSON_AddObjectToObject(o, "tailscale");
    cJSON_AddBoolToObject(t, "enabled", g_cfg.ts_enabled);
    if (!s_ml) {
        cJSON_AddStringToObject(t, "state", g_cfg.ts_enabled ? "waiting_wifi" : "disabled");
        return;
    }
    cJSON_AddStringToObject(t, "state", state_name(microlink_get_state(s_ml)));
    char ip[16] = "";
    uint32_t vpn_ip = microlink_get_vpn_ip(s_ml);
    if (vpn_ip) {
        microlink_ip_to_str(vpn_ip, ip);
    }
    cJSON_AddStringToObject(t, "ip", ip);
    cJSON_AddStringToObject(t, "hostname", g_cfg.ts_hostname);
    cJSON_AddNumberToObject(t, "peers", microlink_get_peer_count(s_ml));
    cJSON_AddBoolToObject(t, "auth_error", microlink_auth_failed(s_ml));
}
