#include "net.h"

#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include "app_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_tls_errors.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "lwip/def.h"
#include "leds.h"
#include "mqtt_client.h"
#include "ota.h"
#include "provision.h"
#include "tailscale.h"
#include "status_led.h"
#include "webui.h"
#include "ha.h"

static const char *TAG = "net";

#define HA_PREFIX "homeassistant"

static esp_netif_t *s_netif;
static esp_mqtt_client_handle_t s_mqtt;
static bool s_wifi_up;
static bool s_mqtt_up;
static bool s_mqtt_refused;
static int s_retry;
static TimerHandle_t s_retry_timer;
static uint32_t s_wifi_drops;       // 繋がっていた Wi-Fi が切れた回数（起動してから）
static uint32_t s_mqtt_connects;    // MQTT に繋がった回数（2 回目以降が再接続）

static char s_base[48];           // headboard/<id>
static char s_avail_topic[64];    // headboard/<id>/status
static char s_mqtt_addr[64];      // 接続先（tailnet の名前は IP に変換したもの）
static char s_mqtt_error[160];    // MQTT に繋がらない理由（設定ページに表示）

// ---------------------------------------------------------------- MQTT

static void topic_for(light_id_t id, const char *suffix, char *buf, size_t len)
{
    snprintf(buf, len, "%s/%s/%s", s_base, leds_key(id), suffix);
}

static void publish_state(light_id_t id)
{
    if (!s_mqtt_up) {
        return;
    }
    char topic[80];
    topic_for(id, "state", topic, sizeof(topic));
    char *payload = leds_state_json(id);
    esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 1);
    free(payload);
}

// Home Assistant の MQTT 自動探索（discovery）用の設定を送る
static void publish_discovery(void)
{
    const char *dev_id = config_device_id();
    for (int i = 0; i < LIGHT_COUNT; i++) {
        char uid[48], cmd_t[80], stat_t[80], topic[128];
        snprintf(uid, sizeof(uid), "%s_%s", dev_id, leds_key(i));
        topic_for(i, "set", cmd_t, sizeof(cmd_t));
        topic_for(i, "state", stat_t, sizeof(stat_t));
        snprintf(topic, sizeof(topic), HA_PREFIX "/light/%s/%s/config", dev_id, leds_key(i));

        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", leds_display_name(i));
        cJSON_AddStringToObject(o, "unique_id", uid);
        cJSON_AddStringToObject(o, "schema", "json");
        cJSON_AddStringToObject(o, "command_topic", cmd_t);
        cJSON_AddStringToObject(o, "state_topic", stat_t);
        cJSON_AddStringToObject(o, "availability_topic", s_avail_topic);
        cJSON *modes = cJSON_AddArrayToObject(o, "supported_color_modes");
        cJSON_AddItemToArray(modes, cJSON_CreateString("brightness"));
        cJSON_AddNumberToObject(o, "brightness_scale", 255);
        cJSON_AddStringToObject(o, "icon", i == LIGHT_READING ? "mdi:book-open-variant" : "mdi:led-strip-variant");

        ha_add_device(o);

        char *payload = cJSON_PrintUnformatted(o);
        esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 1);
        free(payload);
        cJSON_Delete(o);
    }
}

static void on_mqtt_connected(void)
{
    s_mqtt_up = true;
    s_mqtt_refused = false;
    s_mqtt_error[0] = 0;
    s_mqtt_connects++;
    ESP_LOGI(TAG, "MQTT connected");
    esp_mqtt_client_publish(s_mqtt, s_avail_topic, "online", 0, 1, 1);
    publish_discovery();
    for (int i = 0; i < LIGHT_COUNT; i++) {
        char topic[80];
        topic_for(i, "set", topic, sizeof(topic));
        esp_mqtt_client_subscribe(s_mqtt, topic, 1);
        publish_state(i);
    }
    // Home Assistant 再起動時に自動探索の設定を送り直すため
    esp_mqtt_client_subscribe(s_mqtt, HA_PREFIX "/status", 1);
    ha_on_mqtt_connected(s_mqtt, s_base, s_avail_topic);
}

static void on_mqtt_data(esp_mqtt_event_handle_t ev)
{
    char topic[96];
    int tl = ev->topic_len < (int)sizeof(topic) - 1 ? ev->topic_len : (int)sizeof(topic) - 1;
    memcpy(topic, ev->topic, tl);
    topic[tl] = 0;

    if (strcmp(topic, HA_PREFIX "/status") == 0) {
        if (ev->data_len == 6 && strncmp(ev->data, "online", 6) == 0) {
            ESP_LOGI(TAG, "Home Assistant online, re-sending discovery");
            publish_discovery();
            for (int i = 0; i < LIGHT_COUNT; i++) {
                publish_state(i);
            }
            ha_on_mqtt_connected(s_mqtt, s_base, s_avail_topic);
        }
        return;
    }
    if (ha_handle_message(topic, ev->data, ev->data_len)) {
        return;
    }
    for (int i = 0; i < LIGHT_COUNT; i++) {
        char set_t[80];
        topic_for(i, "set", set_t, sizeof(set_t));
        if (strcmp(topic, set_t) == 0) {
            ESP_LOGI(TAG, "%s <- %.*s", leds_key(i), ev->data_len, ev->data);
            leds_apply_command(i, ev->data, ev->data_len);
            status_led_pulse();
            return;
        }
    }
}

static void set_mqtt_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_mqtt_error, sizeof(s_mqtt_error), fmt, ap);
    va_end(ap);
}

static const char *refused_reason(int code)
{
    switch (code) {
    case MQTT_CONNECTION_REFUSE_PROTOCOL: return "ブローカーが MQTT のバージョンに対応していません";
    case MQTT_CONNECTION_REFUSE_ID_REJECTED: return "クライアント ID を拒否されました";
    case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE: return "ブローカーが利用できない状態です";
    case MQTT_CONNECTION_REFUSE_BAD_USERNAME: return "ユーザー名かパスワードが違います";
    case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED: return "認証されませんでした（ユーザー名・パスワードを確認してください）";
    default: return "接続を拒否されました";
    }
}

// MQTT_EVENT_ERROR の中身を、設定ページに出す説明に変える
static void record_mqtt_error(const esp_mqtt_error_codes_t *e)
{
    if (!e) {
        set_mqtt_error("不明なエラー");
        return;
    }
    if (e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        ESP_LOGW(TAG, "MQTT connection refused (code %d)", e->connect_return_code);
        set_mqtt_error("ブローカーに接続を拒否されました: %s", refused_reason(e->connect_return_code));
        s_mqtt_refused = true;
        return;
    }
    int err = e->esp_tls_last_esp_err;
    int sock = e->esp_transport_sock_errno;
    ESP_LOGW(TAG, "MQTT transport error: esp_err=0x%x, errno=%d (%s)", err, sock, sock ? strerror(sock) : "-");
    if (err == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) {
        set_mqtt_error("ホスト名「%s」を IP アドレスに変換できません。IP アドレスで指定してみてください", s_mqtt_addr);
    } else if (sock == ECONNREFUSED) {
        set_mqtt_error("%s:%u に接続を断られました。ポート番号が違うか、ブローカーが動いていません", s_mqtt_addr, g_cfg.mqtt_port);
    } else if (sock == EHOSTUNREACH || sock == ENETUNREACH) {
        set_mqtt_error("%s に届きません。IP アドレスとネットワークを確認してください", s_mqtt_addr);
    } else if (err == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT || sock == ETIMEDOUT || sock == EAGAIN) {
        set_mqtt_error("%s:%u から応答がありません。IP アドレス・ポート・ファイアウォールを確認してください", s_mqtt_addr, g_cfg.mqtt_port);
    } else if (sock == ECONNRESET || sock == ECONNABORTED) {
        set_mqtt_error("%s:%u との接続が切られました。ポート番号が違うか、ブローカーが動いていない可能性があります", s_mqtt_addr, g_cfg.mqtt_port);
    } else {
        set_mqtt_error("通信エラー（esp_err=0x%x, errno=%d）", err, sock);
    }
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        on_mqtt_connected();
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (s_mqtt_up) {
            ESP_LOGW(TAG, "MQTT disconnected");
            set_mqtt_error("ブローカーとの接続が切れました（再接続中）");
        }
        s_mqtt_up = false;
        ha_on_mqtt_disconnected();
        break;
    case MQTT_EVENT_DATA:
        on_mqtt_data(ev);
        break;
    case MQTT_EVENT_ERROR:
        record_mqtt_error(ev->error_handle);
        break;
    default:
        break;
    }
}

static bool parse_ipv4(const char *s, uint32_t *host_order)
{
    esp_ip4_addr_t a;
    if (esp_netif_str_to_ip4(s, &a) != ESP_OK) {
        return false;
    }
    *host_order = lwip_ntohl(a.addr);
    return true;
}

// ブローカーに繋ぐのに tailnet が必要か。
// LAN の IP アドレスなら不要、tailnet の IP（100.64.0.0/10）や名前なら Tailscale の接続を待つ
static bool mqtt_needs_tailnet(void)
{
    if (!g_cfg.ts_enabled) {
        return false;
    }
    uint32_t ip;
    if (!parse_ipv4(g_cfg.mqtt_host, &ip)) {
        return true;
    }
    return (ip & 0xFFC00000u) == 0x64400000u;
}

static void mqtt_start(void)
{
    if (s_mqtt) {
        return;  // 一度起動すれば、再接続は MQTT ライブラリが自動で行う
    }
    if (!g_cfg.mqtt_host[0]) {
        ESP_LOGW(TAG, "MQTT broker is not configured");
        set_mqtt_error("MQTT ブローカーが設定されていません");
        return;
    }
    // tailnet のホスト名（MagicDNS）なら VPN の IP に変換する。見つからなければ通常の DNS に任せる
    if (!ts_resolve(g_cfg.mqtt_host, s_mqtt_addr, sizeof(s_mqtt_addr))) {
        strlcpy(s_mqtt_addr, g_cfg.mqtt_host, sizeof(s_mqtt_addr));
    }
    esp_mqtt_client_config_t cfg = {
        .broker.address.hostname = s_mqtt_addr,
        .broker.address.port = g_cfg.mqtt_port,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.client_id = config_device_id(),
        .credentials.username = g_cfg.mqtt_user[0] ? g_cfg.mqtt_user : NULL,
        .credentials.authentication.password = g_cfg.mqtt_pass[0] ? g_cfg.mqtt_pass : NULL,
        .session.last_will.topic = s_avail_topic,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .session.keepalive = 30,
        .buffer.size = 2048,  // 自動探索の設定と診断情報が 1 KB 前後になる
        .network.reconnect_timeout_ms = 5000,
    };
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt);
    ESP_LOGI(TAG, "MQTT connecting to %s:%u", s_mqtt_addr, g_cfg.mqtt_port);
    set_mqtt_error("%s:%u に接続しています…", s_mqtt_addr, g_cfg.mqtt_port);
}

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
        if (g_cfg.ts_enabled) {
            ts_on_wifi_up(mqtt_start);
        }
        if (!mqtt_needs_tailnet()) {
            mqtt_start();
        } else {
            ESP_LOGI(TAG, "MQTT will start after Tailscale connects");
            set_mqtt_error("Tailscale の接続を待っています（ブローカー: %s）", g_cfg.mqtt_host);
        }
    }
}

static void on_led_changed(light_id_t id)
{
    publish_state(id);
}

void net_start(void)
{
    snprintf(s_base, sizeof(s_base), "headboard/%s", config_device_id());
    snprintf(s_avail_topic, sizeof(s_avail_topic), "%s/status", s_base);
    leds_set_changed_cb(on_led_changed);

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
    cJSON_AddBoolToObject(o, "mqtt_connected", s_mqtt_up);
    char addr[80] = "";
    if (s_mqtt_addr[0]) {
        snprintf(addr, sizeof(addr), "%s:%u", s_mqtt_addr, g_cfg.mqtt_port);
    }
    cJSON_AddStringToObject(o, "mqtt_addr", addr);
    cJSON_AddStringToObject(o, "mqtt_error", s_mqtt_up ? "" : s_mqtt_error);
    cJSON_AddStringToObject(o, "mqtt_base_topic", s_base);
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
    cJSON_AddNumberToObject(o, "mqtt_reconnects", s_mqtt_connects > 0 ? s_mqtt_connects - 1 : 0);
}

void net_get_state(net_state_t *st)
{
    st->wifi_configured = g_cfg.wifi_ssid[0] != 0;
    st->wifi_up = s_wifi_up;
    st->mqtt_configured = g_cfg.mqtt_host[0] != 0;
    st->mqtt_up = s_mqtt_up;
    st->mqtt_refused = s_mqtt_refused;
    st->ble_setup = prov_is_active();
}
