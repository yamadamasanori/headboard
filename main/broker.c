#include "broker.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_tls_errors.h"
#include "ha.h"
#include "leds.h"
#include "lwip/def.h"
#include "mbedtls/x509.h"
#include "status_led.h"
#include "tailscale.h"

static const char *TAG = "broker";

static esp_mqtt_client_handle_t s_mqtt;
static bool s_mqtt_up;
static bool s_mqtt_refused;
static uint32_t s_mqtt_connects;    // MQTT に繋がった回数（2 回目以降が再接続）

static char s_base[48];           // headboard/<id>
static char s_avail_topic[64];    // headboard/<id>/status
static char s_mqtt_addr[64];      // 接続先（tailnet の名前は IP に変換したもの）
static char s_mqtt_error[160];    // MQTT に繋がらない理由（設定ページに表示）

// ---------------------------------------------------------------- ライト

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

// ---------------------------------------------------------------- 接続と受信

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
    ha_on_mqtt_connected();
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
            ha_on_mqtt_connected();
        }
        return;
    }
    if (ha_handle_message(topic, ev->data, ev->data_len, ev->retain)) {
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
    int vflags = e->esp_tls_cert_verify_flags;
    ESP_LOGW(TAG, "MQTT transport error: esp_err=0x%x, tls=-0x%x, verify=0x%x, errno=%d (%s)", err,
             (unsigned)-e->esp_tls_stack_err, (unsigned)vflags, sock, sock ? strerror(sock) : "-");
    if (vflags & MBEDTLS_X509_BADCERT_CN_MISMATCH) {
        set_mqtt_error("ブローカーの証明書に「%s」が入っていません。証明書を作ったときの名前・IP で指定してください", g_cfg.mqtt_host);
    } else if (vflags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) {
        set_mqtt_error(g_cfg.mqtt_ca[0] ? "ブローカーの証明書が、設定した CA 証明書で確かめられません。ca.crt を貼り直してください"
                                        : "ブローカーの証明書を確かめられません。自分で作った証明書なら、CA 証明書（ca.crt）を設定してください");
    } else if (vflags) {
        set_mqtt_error("ブローカーの証明書を確かめられません（0x%x）", (unsigned)vflags);
    } else if (g_cfg.mqtt_tls && (err == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED || err == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST)
               && sock == 0) {
        set_mqtt_error("%s:%u と TLS で接続できません。TLS 用のポート（通常は 8883）か確認してください", s_mqtt_addr, g_cfg.mqtt_port);
    } else if (err == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) {
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
        .broker.address.transport = g_cfg.mqtt_tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP,
        // 証明書の確認。自分で作った CA があればそれで、なければ公開の認証局（証明書バンドル）で確かめる。
        // 照合する名前は、設定に書いたもの（tailnet の名前を IP に変換していても、元の名前で照合する）
        .broker.verification.certificate = g_cfg.mqtt_tls && g_cfg.mqtt_ca[0] ? g_cfg.mqtt_ca : NULL,
        .broker.verification.crt_bundle_attach = g_cfg.mqtt_tls && !g_cfg.mqtt_ca[0] ? esp_crt_bundle_attach : NULL,
        .broker.verification.common_name = g_cfg.mqtt_tls ? g_cfg.mqtt_host : NULL,
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
    if (!s_mqtt) {
        // 次に Wi-Fi か Tailscale が繋がったときに、もう一度試す
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        set_mqtt_error("MQTT を開始できませんでした（メモリ不足など）");
        return;
    }
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt);
    ESP_LOGI(TAG, "MQTT connecting to %s:%u%s", s_mqtt_addr, g_cfg.mqtt_port, g_cfg.mqtt_tls ? " (TLS)" : "");
    set_mqtt_error("%s:%u に接続しています…", s_mqtt_addr, g_cfg.mqtt_port);
}

static void on_led_changed(light_id_t id)
{
    publish_state(id);
}

// ---------------------------------------------------------------- 公開する関数

void broker_init(void)
{
    snprintf(s_base, sizeof(s_base), "headboard/%s", config_device_id());
    snprintf(s_avail_topic, sizeof(s_avail_topic), "%s/status", s_base);
    leds_set_changed_cb(on_led_changed);
}

void broker_on_wifi_up(void)
{
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

esp_mqtt_client_handle_t broker_client(void)
{
    return s_mqtt;
}

const char *broker_base_topic(void)
{
    return s_base;
}

const char *broker_avail_topic(void)
{
    return s_avail_topic;
}

bool broker_is_up(void)
{
    return s_mqtt_up;
}

bool broker_is_refused(void)
{
    return s_mqtt_refused;
}

uint32_t broker_reconnects(void)
{
    return s_mqtt_connects > 0 ? s_mqtt_connects - 1 : 0;
}

void broker_status_to_json(cJSON *o)
{
    cJSON_AddBoolToObject(o, "mqtt_connected", s_mqtt_up);
    cJSON_AddBoolToObject(o, "mqtt_tls", g_cfg.mqtt_tls);
    char addr[80] = "";
    if (s_mqtt_addr[0]) {
        snprintf(addr, sizeof(addr), "%s:%u", s_mqtt_addr, g_cfg.mqtt_port);
    }
    cJSON_AddStringToObject(o, "mqtt_addr", addr);
    cJSON_AddStringToObject(o, "mqtt_error", s_mqtt_up ? "" : s_mqtt_error);
    cJSON_AddStringToObject(o, "mqtt_base_topic", s_base);
}
