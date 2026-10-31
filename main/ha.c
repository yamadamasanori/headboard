#include "ha.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "broker.h"
#include "driver/temperature_sensor.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logbuf.h"
#include "net.h"
#include "restart.h"
#include "usage.h"
#include "status_led.h"
#include "tailscale.h"

static const char *TAG = "ha";

#define DIAG_INTERVAL_MS  60000
#define CHECK_INTERVAL_US (6LL * 3600 * 1000000)   // 新しい版を探す間隔
#define IDENTIFY_MS       10000
#define UPDATE_URL        CONFIG_HEADBOARD_UPDATE_URL
#define RELEASE_URL       CONFIG_HEADBOARD_RELEASE_URL
#define OTA_FILE          "firmware/headboard-led-ota.bin"

// ha_task への依頼
#define REQ_PUBLISH   (1u << 0)
#define REQ_CHECK     (1u << 1)
#define REQ_INSTALL   (1u << 2)
#define REQ_RESTART   (1u << 3)
#define REQ_CHECK_NOW (1u << 4)   // 「更新を確認」ボタン（前回の確認からの時間によらず確かめる）

static TaskHandle_t s_task;
static volatile bool s_connected;
static temperature_sensor_handle_t s_tsens;

static char s_latest[32];          // 配布元の最新の版（分からなければ空）
static volatile bool s_installing;
static volatile int s_progress = -1;

// ---------------------------------------------------------------- 自動探索

typedef enum {
    CAT_NONE,
    CAT_DIAG,     // 診断（デバイスのページの「診断」欄）
    CAT_CONFIG,   // 設定（デバイスのページの「設定」欄）
    CAT_CONFIG_OFF,  // 設定のうち、普段使わず誤操作を避けたいもの（利用者が有効にするまで使えない）
} category_t;

typedef enum {
    WHEN_ALWAYS,
    WHEN_TAILSCALE,   // Tailscale を使うときだけ
    WHEN_UPDATE,      // 配布元が設定されているときだけ
} when_t;

typedef struct {
    const char *component;   // sensor / binary_sensor / button / number / switch / update
    const char *key;         // unique_id とトピックに使う
    const char *name;
    category_t cat;
    when_t when;
    const char *value;       // value_template（state_topic は <base>/diag）
    const char *device_class;
    const char *unit;
    const char *state_class;
    int precision;           // suggested_display_precision（-1 で指定しない）
    const char *icon;
} entity_t;

#define V(x) "{{ value_json." x " }}"
#define ON_OFF(x) "{{ 'ON' if value_json." x " else 'OFF' }}"
#define OR_NONE(x) "{{ value_json." x " or 'なし' }}"

static const entity_t ENTITIES[] = {
    // 診断
    {"sensor", "rssi", "Wi-Fi の電波の強さ", CAT_DIAG, WHEN_ALWAYS, V("rssi"), "signal_strength", "dBm", "measurement", 0, NULL},
    {"sensor", "ip", "IP アドレス", CAT_DIAG, WHEN_ALWAYS, V("ip"), NULL, NULL, NULL, -1, "mdi:ip-network"},
    {"sensor", "wifi_ssid", "Wi-Fi の SSID", CAT_DIAG, WHEN_ALWAYS, V("wifi_ssid"), NULL, NULL, NULL, -1, "mdi:wifi"},
    {"sensor", "wifi_bssid", "Wi-Fi のアクセスポイント", CAT_DIAG, WHEN_ALWAYS, V("wifi_bssid"), NULL, NULL, NULL, -1, "mdi:access-point"},
    {"sensor", "wifi_channel", "Wi-Fi のチャンネル", CAT_DIAG, WHEN_ALWAYS, V("wifi_channel"), NULL, NULL, NULL, 0, "mdi:wifi-cog"},
    {"sensor", "uptime", "稼働時間", CAT_DIAG, WHEN_ALWAYS, V("uptime_s"), "duration", "s", NULL, 0, NULL},
    {"sensor", "reset_reason", "前回の再起動の理由", CAT_DIAG, WHEN_ALWAYS, V("reset_reason"), NULL, NULL, NULL, -1, "mdi:restart-alert"},
    {"binary_sensor", "abnormal_reset", "前回の再起動が異常", CAT_DIAG, WHEN_ALWAYS, V("abnormal_reset"), "problem", NULL, NULL, -1, NULL},
    {"sensor", "last_error", "直前のエラー", CAT_DIAG, WHEN_ALWAYS, OR_NONE("last_error"), NULL, NULL, NULL, -1, "mdi:alert-circle-outline"},
    {"sensor", "prev_error", "前回の起動の最後のエラー", CAT_DIAG, WHEN_ALWAYS, OR_NONE("prev_error"), NULL, NULL, NULL, -1, "mdi:alert-circle-outline"},
    {"sensor", "free_heap", "空きメモリ", CAT_DIAG, WHEN_ALWAYS, V("free_heap"), "data_size", "B", "measurement", 0, NULL},
    {"sensor", "min_free_heap", "空きメモリの最小値", CAT_DIAG, WHEN_ALWAYS, V("min_free_heap"), "data_size", "B", "measurement", 0, NULL},
    {"sensor", "free_internal", "内蔵 RAM の空き", CAT_DIAG, WHEN_ALWAYS, V("free_internal"), "data_size", "B", "measurement", 0, NULL},
    {"sensor", "min_free_internal", "内蔵 RAM の空きの最小値", CAT_DIAG, WHEN_ALWAYS, V("min_free_internal"), "data_size", "B", "measurement", 0, NULL},
    {"sensor", "free_psram", "PSRAM の空き", CAT_DIAG, WHEN_ALWAYS, V("free_psram"), "data_size", "B", "measurement", 0, NULL},
    {"sensor", "temperature", "チップの温度", CAT_DIAG, WHEN_ALWAYS, V("temperature"), "temperature", "°C", "measurement", 1, NULL},
    {"sensor", "status_led", "状態表示 LED の表示", CAT_DIAG, WHEN_ALWAYS, V("status_led"), NULL, NULL, NULL, -1, "mdi:led-on"},
    {"sensor", "boot_count", "起動した回数", CAT_DIAG, WHEN_ALWAYS, V("boot_count"), NULL, NULL, "total_increasing", 0, "mdi:counter"},
    {"sensor", "brownout_count", "電圧低下による再起動の回数", CAT_DIAG, WHEN_ALWAYS, V("brownout_count"), NULL, NULL, "total_increasing", 0, "mdi:flash-alert"},
    {"sensor", "abnormal_reset_count", "クラッシュによる再起動の回数", CAT_DIAG, WHEN_ALWAYS, V("abnormal_reset_count"), NULL, NULL, "total_increasing", 0, "mdi:bug"},
    {"sensor", "wifi_disconnects", "Wi-Fi が切れた回数", CAT_DIAG, WHEN_ALWAYS, V("wifi_disconnects"), NULL, NULL, "total_increasing", 0, "mdi:wifi-off"},
    {"sensor", "mqtt_reconnects", "MQTT の再接続の回数", CAT_DIAG, WHEN_ALWAYS, V("mqtt_reconnects"), NULL, NULL, "total_increasing", 0, "mdi:lan-disconnect"},
    {"sensor", "ts_state", "Tailscale の状態", CAT_DIAG, WHEN_TAILSCALE, V("tailscale.state"), NULL, NULL, NULL, -1, "mdi:vpn"},
    {"sensor", "ts_ip", "Tailscale の IP アドレス", CAT_DIAG, WHEN_TAILSCALE, V("tailscale.ip"), NULL, NULL, NULL, -1, "mdi:vpn"},
    {"sensor", "ts_peers", "Tailscale のピアの数", CAT_DIAG, WHEN_TAILSCALE, V("tailscale.peers"), NULL, NULL, "measurement", 0, "mdi:vpn"},
    {"binary_sensor", "ts_auth_error", "Tailscale の認証エラー", CAT_DIAG, WHEN_TAILSCALE, ON_OFF("tailscale.auth_error"), "problem", NULL, NULL, -1, NULL},

    // ライトの累計
    {"sensor", "reading_on_time", "読書灯の点灯時間", CAT_NONE, WHEN_ALWAYS, V("lights.reading.on_time_h"), "duration", "h", "total_increasing", 1, NULL},
    {"sensor", "reading_on_count", "読書灯の点灯回数", CAT_NONE, WHEN_ALWAYS, V("lights.reading.on_count"), NULL, NULL, "total_increasing", 0, "mdi:counter"},
    {"sensor", "reading_power", "読書灯の消費電力", CAT_NONE, WHEN_ALWAYS, V("lights.reading.power_w"), "power", "W", "measurement", 2, NULL},
    {"sensor", "reading_energy", "読書灯の電力量", CAT_NONE, WHEN_ALWAYS, V("lights.reading.energy_wh"), "energy", "Wh", "total_increasing", 1, NULL},
    {"sensor", "backlight_on_time", "バックライトの点灯時間", CAT_NONE, WHEN_ALWAYS, V("lights.backlight.on_time_h"), "duration", "h", "total_increasing", 1, NULL},
    {"sensor", "backlight_on_count", "バックライトの点灯回数", CAT_NONE, WHEN_ALWAYS, V("lights.backlight.on_count"), NULL, NULL, "total_increasing", 0, "mdi:counter"},
    {"sensor", "backlight_power", "バックライトの消費電力", CAT_NONE, WHEN_ALWAYS, V("lights.backlight.power_w"), "power", "W", "measurement", 2, NULL},
    {"sensor", "backlight_energy", "バックライトの電力量", CAT_NONE, WHEN_ALWAYS, V("lights.backlight.energy_wh"), "energy", "Wh", "total_increasing", 1, NULL},

    // 操作（command_topic は <base>/<key>/set）
    {"button", "restart", "再起動", CAT_CONFIG, WHEN_ALWAYS, NULL, "restart", NULL, NULL, -1, NULL},
    {"button", "identify", "本体を光らせる", CAT_CONFIG, WHEN_ALWAYS, NULL, "identify", NULL, NULL, -1, NULL},
    {"button", "check_update", "更新を確認", CAT_CONFIG, WHEN_UPDATE, NULL, NULL, NULL, NULL, -1, "mdi:cloud-refresh"},
    {"button", "reset_usage", "累計をクリア", CAT_CONFIG_OFF, WHEN_ALWAYS, NULL, NULL, NULL, NULL, -1, "mdi:counter"},
    {"number", "sled_brightness", "状態表示 LED の明るさ", CAT_CONFIG, WHEN_ALWAYS, V("cfg.sled_brightness"), NULL, NULL, NULL, -1, "mdi:brightness-6"},
    {"switch", "sled_always_on", "状態表示 LED を常に点灯", CAT_CONFIG, WHEN_ALWAYS, ON_OFF("cfg.sled_always_on"), NULL, NULL, NULL, -1, "mdi:led-on"},
    {"switch", "log_verbose", "詳細ログ", CAT_CONFIG, WHEN_ALWAYS, ON_OFF("cfg.log_verbose"), NULL, NULL, NULL, -1, "mdi:text-box-search-outline"},

    // 更新（state_topic は <base>/update、command_topic は <base>/update/set）
    {"update", "update", "ファームウェア", CAT_CONFIG, WHEN_UPDATE, NULL, "firmware", NULL, NULL, -1, NULL},
};
#define ENTITY_COUNT (sizeof(ENTITIES) / sizeof(ENTITIES[0]))

void ha_add_device(cJSON *o)
{
    cJSON *dev = cJSON_AddObjectToObject(o, "device");
    cJSON *ids = cJSON_AddArrayToObject(dev, "identifiers");
    cJSON_AddItemToArray(ids, cJSON_CreateString(config_device_id()));
    cJSON_AddStringToObject(dev, "name", g_cfg.device_name);
    cJSON_AddStringToObject(dev, "manufacturer", "DIY");
    cJSON_AddStringToObject(dev, "model", "ESP32 Headboard LED");
    cJSON_AddStringToObject(dev, "sw_version", FW_VERSION);
    char url[48];
    snprintf(url, sizeof(url), "http://%s.local/", config_hostname());
    cJSON_AddStringToObject(dev, "configuration_url", url);
}

static bool enabled(const entity_t *e)
{
    switch (e->when) {
    case WHEN_TAILSCALE: return g_cfg.ts_enabled;
    case WHEN_UPDATE: return UPDATE_URL[0] != 0;
    default: return true;
    }
}

static void publish_entity(const entity_t *e)
{
    const char *dev_id = config_device_id();
    char topic[128], buf[96];
    snprintf(topic, sizeof(topic), HA_PREFIX "/%s/%s/%s/config", e->component, dev_id, e->key);
    if (!enabled(e)) {
        // 使わなくなったものは Home Assistant から消す
        esp_mqtt_client_publish(broker_client(), topic, "", 0, 1, 1);
        return;
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", e->name);
    snprintf(buf, sizeof(buf), "%s_%s", dev_id, e->key);
    cJSON_AddStringToObject(o, "unique_id", buf);
    cJSON_AddStringToObject(o, "availability_topic", broker_avail_topic());
    if (e->cat == CAT_DIAG) {
        cJSON_AddStringToObject(o, "entity_category", "diagnostic");
    } else if (e->cat == CAT_CONFIG || e->cat == CAT_CONFIG_OFF) {
        cJSON_AddStringToObject(o, "entity_category", "config");
    }
    if (e->cat == CAT_CONFIG_OFF) {
        cJSON_AddFalseToObject(o, "enabled_by_default");
    }
    if (e->value) {
        snprintf(buf, sizeof(buf), "%s/diag", broker_base_topic());
        cJSON_AddStringToObject(o, "state_topic", buf);
        cJSON_AddStringToObject(o, "value_template", e->value);
    }
    if (strcmp(e->component, "update") == 0) {
        snprintf(buf, sizeof(buf), "%s/update", broker_base_topic());
        cJSON_AddStringToObject(o, "state_topic", buf);
        cJSON_AddStringToObject(o, "payload_install", "install");
        cJSON_AddStringToObject(o, "title", "ヘッドボードLED");
    }
    if (strcmp(e->component, "button") == 0 || strcmp(e->component, "number") == 0 ||
        strcmp(e->component, "switch") == 0 || strcmp(e->component, "update") == 0) {
        snprintf(buf, sizeof(buf), "%s/%s/set", broker_base_topic(), e->key);
        cJSON_AddStringToObject(o, "command_topic", buf);
    }
    if (strcmp(e->component, "number") == 0) {
        cJSON_AddNumberToObject(o, "min", 1);
        cJSON_AddNumberToObject(o, "max", 255);
        cJSON_AddStringToObject(o, "mode", "slider");
    }
    if (e->device_class) {
        cJSON_AddStringToObject(o, "device_class", e->device_class);
    }
    if (e->unit) {
        cJSON_AddStringToObject(o, "unit_of_measurement", e->unit);
    }
    if (e->state_class) {
        cJSON_AddStringToObject(o, "state_class", e->state_class);
    }
    if (e->precision >= 0) {
        cJSON_AddNumberToObject(o, "suggested_display_precision", e->precision);
    }
    if (e->icon) {
        cJSON_AddStringToObject(o, "icon", e->icon);
    }
    ha_add_device(o);

    char *payload = cJSON_PrintUnformatted(o);
    if (payload) {
        esp_mqtt_client_publish(broker_client(), topic, payload, 0, 1, 1);
        free(payload);
    }
    cJSON_Delete(o);
}

// ---------------------------------------------------------------- 状態の送信

static void publish_diag(void)
{
    cJSON *o = cJSON_CreateObject();
    net_diag_to_json(o);
    ts_status_to_json(o);
    usage_to_json(o);
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(o, "reset_reason", logbuf_reset_reason());
    cJSON_AddStringToObject(o, "last_error", logbuf_last_error());
    cJSON_AddStringToObject(o, "prev_error", logbuf_prev_error());
    cJSON_AddNumberToObject(o, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(o, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(o, "free_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "min_free_internal", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    float temp;
    if (s_tsens && temperature_sensor_get_celsius(s_tsens, &temp) == ESP_OK) {
        cJSON_AddNumberToObject(o, "temperature", (int)(temp * 10.0f + (temp < 0 ? -0.5f : 0.5f)) / 10.0);
    }
    cJSON_AddStringToObject(o, "status_led", status_led_indicator());
    cJSON *cfg = cJSON_AddObjectToObject(o, "cfg");
    cJSON_AddNumberToObject(cfg, "sled_brightness", g_cfg.sled_brightness);
    cJSON_AddBoolToObject(cfg, "sled_always_on", g_cfg.sled_always_on);
    cJSON_AddBoolToObject(cfg, "log_verbose", g_cfg.log_verbose);

    char topic[64];
    snprintf(topic, sizeof(topic), "%s/diag", broker_base_topic());
    char *payload = cJSON_PrintUnformatted(o);
    if (payload) {
        esp_mqtt_client_publish(broker_client(), topic, payload, 0, 0, 1);
        free(payload);
    }
    cJSON_Delete(o);
}

static void publish_update_state(void)
{
    if (!UPDATE_URL[0]) {
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "installed_version", FW_VERSION);
    if (s_latest[0]) {
        cJSON_AddStringToObject(o, "latest_version", s_latest);
    } else {
        cJSON_AddNullToObject(o, "latest_version");  // まだ分からない
    }
    if (RELEASE_URL[0]) {
        // 最新の版のお知らせ（分からなければ今の版）。版が変わるので状態と一緒に送る
        char url[160];
        snprintf(url, sizeof(url), "%s%s", RELEASE_URL, s_latest[0] ? s_latest : FW_VERSION);
        cJSON_AddStringToObject(o, "release_url", url);
    }
    cJSON_AddBoolToObject(o, "in_progress", s_installing);
    if (s_installing && s_progress >= 0) {
        cJSON_AddNumberToObject(o, "update_percentage", s_progress);
    } else {
        cJSON_AddNullToObject(o, "update_percentage");
    }
    char topic[64];
    snprintf(topic, sizeof(topic), "%s/update", broker_base_topic());
    char *payload = cJSON_PrintUnformatted(o);
    if (payload) {
        esp_mqtt_client_publish(broker_client(), topic, payload, 0, 1, 1);
        free(payload);
    }
    cJSON_Delete(o);
}

// ---------------------------------------------------------------- ファームウェアの更新

// "2.0.10" のような版を比べる（a が新しければ正）
static int compare_version(const char *a, const char *b)
{
    int va[3] = {0}, vb[3] = {0};
    sscanf(a, "%d.%d.%d", &va[0], &va[1], &va[2]);
    sscanf(b, "%d.%d.%d", &vb[0], &vb[1], &vb[2]);
    for (int i = 0; i < 3; i++) {
        if (va[i] != vb[i]) {
            return va[i] - vb[i];
        }
    }
    return 0;
}

// 配布元の manifest.json から最新の版を読む
static void check_latest(void)
{
    char url[160];
    snprintf(url, sizeof(url), "%smanifest.json", UPDATE_URL);
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return;
    }
    char *body = NULL;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status == 200) {
            const int max = 4096;
            body = malloc(max + 1);
            int len = 0, n;
            while (body && len < max && (n = esp_http_client_read(c, body + len, max - len)) > 0) {
                len += n;
            }
            if (body) {
                body[len] = 0;
            }
        } else {
            ESP_LOGW(TAG, "update check: HTTP %d", status);
        }
    } else {
        ESP_LOGW(TAG, "update check: cannot connect to %s", url);
    }
    esp_http_client_cleanup(c);

    cJSON *m = body ? cJSON_Parse(body) : NULL;
    const cJSON *v = m ? cJSON_GetObjectItemCaseSensitive(m, "version") : NULL;
    if (cJSON_IsString(v) && strlen(v->valuestring) < sizeof(s_latest)) {
        if (strcmp(s_latest, v->valuestring) != 0) {
            ESP_LOGI(TAG, "latest firmware: %s (running %s)", v->valuestring, FW_VERSION);
        }
        strlcpy(s_latest, v->valuestring, sizeof(s_latest));
    } else if (body) {
        ESP_LOGW(TAG, "update check: manifest.json has no version");
    }
    cJSON_Delete(m);
    free(body);
}

static void install_update(void)
{
    if (!s_latest[0] || compare_version(s_latest, FW_VERSION) <= 0) {
        ESP_LOGW(TAG, "no newer firmware to install (latest %s, running %s)", s_latest[0] ? s_latest : "?", FW_VERSION);
        return;
    }
    char url[160];
    snprintf(url, sizeof(url), "%s" OTA_FILE, UPDATE_URL);
    ESP_LOGI(TAG, "installing %s from %s", s_latest, url);
    s_installing = true;
    s_progress = 0;
    publish_update_state();

    esp_http_client_config_t http = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = {.http_config = &http};
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err == ESP_OK) {
        // 書き込む前に、このプロジェクトのファームウェアかを確かめる
        esp_app_desc_t desc;
        err = esp_https_ota_get_img_desc(h, &desc);
        if (err == ESP_OK && strncmp(desc.project_name, esp_app_get_description()->project_name,
                                     sizeof(desc.project_name)) != 0) {
            ESP_LOGE(TAG, "update rejected: different project (%.32s)", desc.project_name);
            err = ESP_ERR_INVALID_VERSION;
        }
    }
    int total = h ? esp_https_ota_get_image_size(h) : -1;
    while (err == ESP_OK) {
        err = esp_https_ota_perform(h);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        err = ESP_OK;
        int done = esp_https_ota_get_image_len_read(h);
        int pct = total > 0 ? (int)((int64_t)done * 100 / total) : -1;
        if (pct >= 0 && pct / 10 != s_progress / 10) {  // 10% ごとに知らせる
            s_progress = pct;
            publish_update_state();
        }
    }
    if (err == ESP_OK && !esp_https_ota_is_complete_data_received(h)) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        err = esp_https_ota_finish(h);
        h = NULL;
    }
    if (h) {
        esp_https_ota_abort(h);
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "update installed, restarting");
        s_progress = 100;
        publish_update_state();
        vTaskDelay(pdMS_TO_TICKS(1000));  // 状態を送り終えるのを待つ
        restart_now();
    }
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        ESP_LOGE(TAG, "update rejected: not signed with the trusted key, or corrupted");
    } else {
        ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
    }
    s_installing = false;
    s_progress = -1;
    publish_update_state();
}

// ---------------------------------------------------------------- タスク

static void ha_task(void *arg)
{
    int64_t last_check_us = 0;
    bool checked = false;
    for (;;) {
        uint32_t req = 0;
        xTaskNotifyWait(0, UINT32_MAX, &req, pdMS_TO_TICKS(DIAG_INTERVAL_MS));
        if (req & REQ_RESTART) {
            ESP_LOGW(TAG, "restart requested from Home Assistant");
            vTaskDelay(pdMS_TO_TICKS(300));
            restart_now();
        }
        if (!s_connected) {
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (UPDATE_URL[0] && (((req & REQ_CHECK) && !checked) || (req & REQ_CHECK_NOW) ||
                              now - last_check_us >= CHECK_INTERVAL_US)) {
            check_latest();
            checked = true;
            last_check_us = now;
            req |= REQ_PUBLISH;
        }
        if (req & REQ_INSTALL) {
            install_update();
        }
        publish_diag();
        if (req & (REQ_PUBLISH | REQ_INSTALL)) {
            publish_update_state();
        }
    }
}

static void request(uint32_t bits)
{
    if (s_task) {
        xTaskNotify(s_task, bits, eSetBits);
    }
}

void ha_init(void)
{
    temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tc, &s_tsens) == ESP_OK) {
        temperature_sensor_enable(s_tsens);
    } else {
        s_tsens = NULL;
    }
    // HTTPS（更新の確認と書き込み）を使うので、スタックを大きめにする
    xTaskCreate(ha_task, "ha", 8192, NULL, 4, &s_task);
}

void ha_on_mqtt_connected(void)
{
    for (size_t i = 0; i < ENTITY_COUNT; i++) {
        publish_entity(&ENTITIES[i]);
        const char *c = ENTITIES[i].component;
        if (enabled(&ENTITIES[i]) && (strcmp(c, "button") == 0 || strcmp(c, "number") == 0 ||
                                      strcmp(c, "switch") == 0 || strcmp(c, "update") == 0)) {
            char topic[80];
            snprintf(topic, sizeof(topic), "%s/%s/set", broker_base_topic(), ENTITIES[i].key);
            esp_mqtt_client_subscribe(broker_client(), topic, 1);
        }
    }
    s_connected = true;
    request(REQ_PUBLISH | REQ_CHECK);
}

void ha_on_mqtt_disconnected(void)
{
    s_connected = false;
}

// <base>/<key>/set なら key を返す
static const char *command_key(const char *topic, char *key, size_t len)
{
    const char *base = broker_base_topic();
    size_t bl = strlen(base);
    if (!base[0] || strncmp(topic, base, bl) != 0 || topic[bl] != '/') {
        return NULL;
    }
    const char *k = topic + bl + 1;
    const char *slash = strchr(k, '/');
    if (!slash || strcmp(slash, "/set") != 0 || (size_t)(slash - k) >= len) {
        return NULL;
    }
    memcpy(key, k, slash - k);
    key[slash - k] = 0;
    return key;
}

static bool payload_is(const char *data, int len, const char *s)
{
    return len == (int)strlen(s) && strncmp(data, s, len) == 0;
}

// 1 回押すたびに動くもの（ブローカーに保持されていると、繋ぐたびに動いてしまう）
static bool is_action(const char *key)
{
    return strcmp(key, "restart") == 0 || strcmp(key, "identify") == 0 || strcmp(key, "check_update") == 0 ||
           strcmp(key, "reset_usage") == 0 || strcmp(key, "update") == 0;
}

bool ha_handle_message(const char *topic, const char *data, int len, bool retained)
{
    char key[32];
    if (!command_key(topic, key, sizeof(key))) {
        return false;
    }
    if (retained && is_action(key)) {
        // retain 付きで送られたボタンなどは、再起動や更新を繰り返さないように無視する
        ESP_LOGW(TAG, "ignored retained command: %s", key);
        return true;
    }
    bool changed = false;
    if (strcmp(key, "restart") == 0) {
        request(REQ_RESTART);
    } else if (strcmp(key, "identify") == 0) {
        status_led_identify(IDENTIFY_MS);
    } else if (strcmp(key, "check_update") == 0) {
        request(REQ_CHECK_NOW);
    } else if (strcmp(key, "reset_usage") == 0) {
        usage_reset();
        request(REQ_PUBLISH);
    } else if (strcmp(key, "update") == 0) {
        if (payload_is(data, len, "install")) {
            request(REQ_INSTALL);
        }
    } else if (strcmp(key, "sled_brightness") == 0) {
        char buf[8] = "";
        memcpy(buf, data, len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1);
        int v = (int)strtof(buf, NULL);
        if (v >= 1 && v <= 255) {
            config_lock();
            g_cfg.sled_brightness = v;
            config_unlock();
            changed = true;
        }
    } else if (strcmp(key, "sled_always_on") == 0 || strcmp(key, "log_verbose") == 0) {
        bool *dst = strcmp(key, "sled_always_on") == 0 ? &g_cfg.sled_always_on : &g_cfg.log_verbose;
        if (payload_is(data, len, "ON") || payload_is(data, len, "OFF")) {
            config_lock();
            *dst = payload_is(data, len, "ON");
            config_unlock();
            config_apply_log_levels();
            changed = true;
        }
    } else {
        return false;  // ライトなど、ほかのモジュール宛て
    }
    ESP_LOGI(TAG, "%s <- %.*s", key, len, data);
    if (changed) {
        if (config_save() != ESP_OK) {
            ESP_LOGE(TAG, "failed to save settings");
        }
        request(REQ_PUBLISH);
    }
    return true;
}
