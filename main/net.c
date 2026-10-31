#include "net.h"

#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "broker.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "ota.h"
#include "provision.h"
#include "tailscale.h"
#include "webui.h"

static const char *TAG = "net";

static esp_netif_t *s_netif;
static bool s_wifi_up;
static int s_retry;
static TimerHandle_t s_retry_timer;
static uint32_t s_wifi_drops;       // 繋がっていた Wi-Fi が切れた回数（起動してから）

// ---------------------------------------------------------------- Wi-Fi

static void retry_cb(TimerHandle_t t)
{
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // Bluetooth で設定中は、設定の受け付け側（provision.c）が接続する
        if (g_cfg.wifi_ssid[0]) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_up) {
            s_wifi_drops++;
        }
        s_wifi_up = false;
        if (!g_cfg.wifi_ssid[0]) {
            return;
        }
        // 失敗が続くほど間隔を延ばす（最大 60 秒）
        int delay_s = s_retry < 6 ? (1 << s_retry) : 60;
        if (delay_s > 60) {
            delay_s = 60;
        }
        s_retry++;
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d), retry in %ds", d->reason, delay_s);
        xTimerChangePeriod(s_retry_timer, pdMS_TO_TICKS(delay_s * 1000), 0);
        xTimerStart(s_retry_timer, 0);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_wifi_up = true;
        s_retry = 0;
        ota_mark_valid();  // Wi-Fi に繋がったので、更新したファームウェアを正常とみなす
        webui_start();
        broker_on_wifi_up();
    }
}

void net_start(void)
{
    broker_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, config_device_id());
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(1000), pdFALSE, NULL, retry_cb);

    if (!g_cfg.wifi_ssid[0]) {
        ESP_LOGW(TAG, "Wi-Fi is not configured. Set it up over Bluetooth (ESP BLE Provisioning app)");
        ota_mark_valid();
        prov_start();
        return;
    }

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, g_cfg.wifi_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, g_cfg.wifi_pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = g_cfg.wifi_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi connecting to \"%s\"", g_cfg.wifi_ssid);
}

void net_status_to_json(cJSON *o)
{
    cJSON_AddBoolToObject(o, "wifi_connected", s_wifi_up);
    char ip[16] = "";
    if (s_wifi_up && s_netif) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(s_netif, &info) == ESP_OK) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
        }
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            cJSON_AddNumberToObject(o, "rssi", ap.rssi);
        }
    }
    cJSON_AddStringToObject(o, "ip", ip);
    broker_status_to_json(o);
    cJSON_AddStringToObject(o, "hostname", config_hostname());
    cJSON_AddBoolToObject(o, "ble_setup", prov_is_active());
    ts_status_to_json(o);
}

void net_diag_to_json(cJSON *o)
{
    char ip[16] = "";
    wifi_ap_record_t ap;
    bool have_ap = s_wifi_up && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    if (s_wifi_up && s_netif) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(s_netif, &info) == ESP_OK) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
        }
    }
    cJSON_AddStringToObject(o, "ip", ip);
    if (have_ap) {
        char bssid[18];
        snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        cJSON_AddNumberToObject(o, "rssi", ap.rssi);
        cJSON_AddStringToObject(o, "wifi_ssid", (const char *)ap.ssid);
        cJSON_AddStringToObject(o, "wifi_bssid", bssid);
        cJSON_AddNumberToObject(o, "wifi_channel", ap.primary);
    }
    cJSON_AddNumberToObject(o, "wifi_disconnects", s_wifi_drops);
    cJSON_AddNumberToObject(o, "mqtt_reconnects", broker_reconnects());
}

void net_get_state(net_state_t *st)
{
    st->wifi_configured = g_cfg.wifi_ssid[0] != 0;
    st->wifi_up = s_wifi_up;
    st->mqtt_configured = g_cfg.mqtt_host[0] != 0;
    st->mqtt_up = broker_is_up();
    st->mqtt_refused = broker_is_refused();
    st->ble_setup = prov_is_active();
}
