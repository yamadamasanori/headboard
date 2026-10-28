#include "provision.h"

#include <string.h>
#include "app_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

static const char *TAG = "provision";

static volatile bool s_active;
static char s_ssid[33];
static char s_pass[65];

static void on_prov_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_PROV_START:
        ESP_LOGI(TAG, "waiting for Wi-Fi setup over Bluetooth (name %s)", config_prov_name());
        break;
    case WIFI_PROV_CRED_RECV: {
        // 繋がるまでは保存しない（間違ったパスワードを保存すると、Bluetooth で設定し直せなくなるため）
        wifi_sta_config_t *sta = data;
        strlcpy(s_ssid, (const char *)sta->ssid, sizeof(s_ssid));
        strlcpy(s_pass, (const char *)sta->password, sizeof(s_pass));
        ESP_LOGI(TAG, "received Wi-Fi settings for \"%s\", connecting", s_ssid);
        break;
    }
    case WIFI_PROV_CRED_FAIL: {
        wifi_prov_sta_fail_reason_t *r = data;
        ESP_LOGW(TAG, "Wi-Fi connection failed: %s. Waiting for new settings",
                 *r == WIFI_PROV_STA_AUTH_ERROR ? "wrong password" : "access point not found");
        // アプリから設定し直せるようにする
        wifi_prov_mgr_reset_sm_state_on_failure();
        break;
    }
    case WIFI_PROV_CRED_SUCCESS:
        ESP_LOGI(TAG, "Wi-Fi connected, saving settings");
        strlcpy(g_cfg.wifi_ssid, s_ssid, sizeof(g_cfg.wifi_ssid));
        strlcpy(g_cfg.wifi_pass, s_pass, sizeof(g_cfg.wifi_pass));
        if (config_save() != ESP_OK) {
            ESP_LOGE(TAG, "failed to save Wi-Fi settings");
        }
        memset(s_pass, 0, sizeof(s_pass));
        break;
    case WIFI_PROV_END:
        // Bluetooth を止めて、そのメモリを返す
        wifi_prov_mgr_deinit();
        s_active = false;
        ESP_LOGI(TAG, "Bluetooth setup finished");
        break;
    default:
        break;
    }
}

void prov_start(void)
{
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, on_prov_event, NULL));
    wifi_prov_mgr_config_t cfg = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    esp_err_t err = wifi_prov_mgr_init(cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_prov_mgr_init failed: %s", esp_err_to_name(err));
        return;
    }
    // 公式アプリ（ESP BLE Provisioning）で使う「Security 1」。本体ごとのコード（PoP）で暗号化する
    err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, (const void *)config_pop(), config_prov_name(), NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start provisioning failed: %s", esp_err_to_name(err));
        wifi_prov_mgr_deinit();
        return;
    }
    s_active = true;
}

bool prov_is_active(void)
{
    return s_active;
}
