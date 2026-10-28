#include "app_config.h"

#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "ESP32-S3 専用です（idf.py set-target esp32s3）"
#endif

static const char *TAG = "config";
static const char *NS = "cfg";

app_config_t g_cfg;
static char s_device_id[24];

static void set_defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.mqtt_port = 1883;
    strlcpy(g_cfg.device_name, "ヘッドボード", sizeof(g_cfg.device_name));
    g_cfg.pin_reading = 4;
    g_cfg.pin_backlight = 5;
    g_cfg.sled_pin = 48;  // ESP32-S3-DevKitC-1 互換ボードのオンボード RGB LED
    g_cfg.sled_brightness = 32;
    g_cfg.sled_always_on = false;
    g_cfg.pwm_freq = 5000;
    g_cfg.pwm_invert = false;
    g_cfg.ts_enabled = false;
    // Tailscale のホスト名には "_" が使えないので "-" にする
    snprintf(g_cfg.ts_hostname, sizeof(g_cfg.ts_hostname), "headboard-%s", config_device_id() + strlen("headboard_"));
}

const char *config_device_id(void)
{
    if (!s_device_id[0]) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_device_id, sizeof(s_device_id), "headboard_%02x%02x%02x", mac[3], mac[4], mac[5]);
    }
    return s_device_id;
}

const char *config_hostname(void)
{
    static char s[24];
    if (!s[0]) {
        snprintf(s, sizeof(s), "headboard-%s", config_device_id() + strlen("headboard_"));
    }
    return s;
}

const char *config_prov_name(void)
{
    static char s[24];
    if (!s[0]) {
        // 公式アプリ（ESP BLE Provisioning）は既定で "PROV_" で始まる名前を探す
        snprintf(s, sizeof(s), "PROV_HB_%s", config_device_id() + strlen("headboard_"));
    }
    return s;
}

// チップ固有の ID（eFuse の OPTIONAL_UNIQUE_ID）。書き込まれていないチップでは、
// 初回起動時に乱数を作って NVS に残す（設定の初期化では消さない）
static void chip_secret(uint8_t out[16])
{
    memset(out, 0, 16);
    esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, out, 128);
    for (int i = 0; i < 16; i++) {
        if (out[i]) {
            return;
        }
    }
    nvs_handle_t h;
    if (nvs_open("fctry", NVS_READWRITE, &h) != ESP_OK) {
        esp_fill_random(out, 16);
        return;
    }
    size_t len = 16;
    if (nvs_get_blob(h, "secret", out, &len) != ESP_OK || len != 16) {
        esp_fill_random(out, 16);
        nvs_set_blob(h, "secret", out, 16);
        nvs_commit(h);
    }
    nvs_close(h);
}

const char *config_pop(void)
{
    static char s[9];
    if (!s[0]) {
        // 読み間違えやすい 0/O/1/I を除いた 32 文字で 8 文字（40 ビット）
        static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        uint8_t secret[16], hash[32];
        chip_secret(secret);
        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        mbedtls_sha256_update(&ctx, (const uint8_t *)"headboard-pop", 13);
        mbedtls_sha256_update(&ctx, secret, sizeof(secret));
        mbedtls_sha256_finish(&ctx, hash);
        mbedtls_sha256_free(&ctx);
        for (int i = 0; i < 8; i++) {
            s[i] = alphabet[hash[i] & 31];
        }
        s[8] = 0;
    }
    return s;
}

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t len)
{
    size_t sz = len;
    nvs_get_str(h, key, dst, &sz);  // 見つからなければ既定値のまま
}

void config_load(void)
{
    set_defaults();
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved config, using defaults");
        return;
    }
    load_str(h, "wifi_ssid", g_cfg.wifi_ssid, sizeof(g_cfg.wifi_ssid));
    load_str(h, "wifi_pass", g_cfg.wifi_pass, sizeof(g_cfg.wifi_pass));
    load_str(h, "mqtt_host", g_cfg.mqtt_host, sizeof(g_cfg.mqtt_host));
    load_str(h, "mqtt_user", g_cfg.mqtt_user, sizeof(g_cfg.mqtt_user));
    load_str(h, "mqtt_pass", g_cfg.mqtt_pass, sizeof(g_cfg.mqtt_pass));
    load_str(h, "dev_name", g_cfg.device_name, sizeof(g_cfg.device_name));
    nvs_get_u16(h, "mqtt_port", &g_cfg.mqtt_port);
    nvs_get_u8(h, "pin_read", &g_cfg.pin_reading);
    nvs_get_u8(h, "pin_back", &g_cfg.pin_backlight);
    nvs_get_u16(h, "pwm_freq", &g_cfg.pwm_freq);
    uint8_t flag = 0;
    if (nvs_get_u8(h, "pwm_inv", &flag) == ESP_OK) {
        g_cfg.pwm_invert = flag != 0;
    }
    if (nvs_get_u8(h, "ts_en", &flag) == ESP_OK) {
        g_cfg.ts_enabled = flag != 0;
    }
    load_str(h, "ts_key", g_cfg.ts_authkey, sizeof(g_cfg.ts_authkey));
    nvs_get_u8(h, "sled_pin", &g_cfg.sled_pin);
    nvs_get_u8(h, "sled_bri", &g_cfg.sled_brightness);
    if (nvs_get_u8(h, "sled_on", &flag) == ESP_OK) {
        g_cfg.sled_always_on = flag != 0;
    }
    if (nvs_get_u8(h, "log_verb", &flag) == ESP_OK) {
        g_cfg.log_verbose = flag != 0;
    }
    load_str(h, "ts_host", g_cfg.ts_hostname, sizeof(g_cfg.ts_hostname));
    nvs_close(h);
    ESP_LOGI(TAG, "config loaded (ssid=\"%s\", mqtt=%s:%u)", g_cfg.wifi_ssid, g_cfg.mqtt_host, g_cfg.mqtt_port);
}

esp_err_t config_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "wifi_ssid", g_cfg.wifi_ssid);
    nvs_set_str(h, "wifi_pass", g_cfg.wifi_pass);
    nvs_set_str(h, "mqtt_host", g_cfg.mqtt_host);
    nvs_set_str(h, "mqtt_user", g_cfg.mqtt_user);
    nvs_set_str(h, "mqtt_pass", g_cfg.mqtt_pass);
    nvs_set_str(h, "dev_name", g_cfg.device_name);
    nvs_set_u16(h, "mqtt_port", g_cfg.mqtt_port);
    nvs_set_u8(h, "pin_read", g_cfg.pin_reading);
    nvs_set_u8(h, "pin_back", g_cfg.pin_backlight);
    nvs_set_u16(h, "pwm_freq", g_cfg.pwm_freq);
    nvs_set_u8(h, "pwm_inv", g_cfg.pwm_invert ? 1 : 0);
    nvs_set_u8(h, "ts_en", g_cfg.ts_enabled ? 1 : 0);
    nvs_set_str(h, "ts_key", g_cfg.ts_authkey);
    nvs_set_str(h, "ts_host", g_cfg.ts_hostname);
    nvs_set_u8(h, "sled_pin", g_cfg.sled_pin);
    nvs_set_u8(h, "sled_bri", g_cfg.sled_brightness);
    nvs_set_u8(h, "sled_on", g_cfg.sled_always_on ? 1 : 0);
    nvs_set_u8(h, "log_verb", g_cfg.log_verbose ? 1 : 0);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t config_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_all(h);
    err = nvs_commit(h);
    nvs_close(h);
    esp_wifi_restore();
    set_defaults();
    return err;
}

esp_err_t config_clear_wifi(void)
{
    g_cfg.wifi_ssid[0] = 0;
    g_cfg.wifi_pass[0] = 0;
    esp_wifi_restore();  // Wi-Fi ドライバが別に保存している接続先も消す
    return config_save();
}

static void set_ts_reset_flag(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "ts_reset", 1);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool config_take_ts_reset_flag(void)
{
    nvs_handle_t h;
    uint8_t flag = 0;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    if (nvs_get_u8(h, "ts_reset", &flag) == ESP_OK) {
        nvs_erase_key(h, "ts_reset");
        nvs_commit(h);
    }
    nvs_close(h);
    return flag != 0;
}

void config_to_json(cJSON *o)
{
    cJSON_AddStringToObject(o, "device_id", config_device_id());
    cJSON_AddStringToObject(o, "device_name", g_cfg.device_name);
    cJSON_AddStringToObject(o, "wifi_ssid", g_cfg.wifi_ssid);
    cJSON_AddBoolToObject(o, "wifi_pass_set", g_cfg.wifi_pass[0] != 0);
    cJSON_AddStringToObject(o, "mqtt_host", g_cfg.mqtt_host);
    cJSON_AddNumberToObject(o, "mqtt_port", g_cfg.mqtt_port);
    cJSON_AddStringToObject(o, "mqtt_user", g_cfg.mqtt_user);
    cJSON_AddBoolToObject(o, "mqtt_pass_set", g_cfg.mqtt_pass[0] != 0);
    cJSON_AddNumberToObject(o, "pin_reading", g_cfg.pin_reading);
    cJSON_AddNumberToObject(o, "pin_backlight", g_cfg.pin_backlight);
    cJSON_AddNumberToObject(o, "pwm_freq", g_cfg.pwm_freq);
    cJSON_AddBoolToObject(o, "pwm_invert", g_cfg.pwm_invert);
    cJSON_AddBoolToObject(o, "ts_enabled", g_cfg.ts_enabled);
    cJSON_AddBoolToObject(o, "ts_authkey_set", g_cfg.ts_authkey[0] != 0);
    cJSON_AddStringToObject(o, "ts_hostname", g_cfg.ts_hostname);
    cJSON_AddNumberToObject(o, "sled_pin", g_cfg.sled_pin == STATUS_LED_DISABLED ? -1 : g_cfg.sled_pin);
    cJSON_AddNumberToObject(o, "sled_brightness", g_cfg.sled_brightness);
    cJSON_AddBoolToObject(o, "sled_always_on", g_cfg.sled_always_on);
    cJSON_AddBoolToObject(o, "log_verbose", g_cfg.log_verbose);
}

void config_apply_log_levels(void)
{
    // Tailscale（MicroLink）は接続処理の途中経過を大量に出すので、通常は警告以上だけにする
    static const char *noisy[] = {
        "microlink", "ml_coord", "ml_derp", "ml_h2", "ml_net_io", "ml_noise", "ml_peer_nvs",
        "ml_stun", "ml_tcp", "ml_udp", "ml_wg_mgr", "ml_zc", "nacl_box", "wifi", "wifi_init", "phy_init",
    };
    esp_log_level_t lv = g_cfg.log_verbose ? ESP_LOG_INFO : ESP_LOG_WARN;
    for (size_t i = 0; i < sizeof(noisy) / sizeof(noisy[0]); i++) {
        esp_log_level_set(noisy[i], lv);
    }
}

static bool valid_hostname(const char *s)
{
    if (!s[0] || s[0] == '-') {
        return false;
    }
    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-')) {
            return false;
        }
    }
    return true;
}

static bool valid_output_pin(int pin)
{
    if (pin < 0 || !GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        return false;
    }
    if (pin >= 26 && pin <= 32) {  // SPI フラッシュ接続ピン
        return false;
    }
#if CONFIG_SPIRAM_MODE_OCT
    if (pin >= 33 && pin <= 37) {  // オクタル PSRAM 接続ピン
        return false;
    }
#endif
    if (pin == 19 || pin == 20 || pin == 43 || pin == 44) {  // USB 端子 / COM 端子
        return false;
    }
    return true;
}

static bool take_str(const cJSON *o, const char *key, char *dst, size_t len, char *err, size_t err_len)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!v) {
        return true;
    }
    if (!cJSON_IsString(v) || strlen(v->valuestring) >= len) {
        snprintf(err, err_len, "%s が不正です（最大 %u 文字）", key, (unsigned)(len - 1));
        return false;
    }
    strlcpy(dst, v->valuestring, len);
    return true;
}

static bool take_int(const cJSON *o, const char *key, int min, int max, int *out, char *err, size_t err_len)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!v) {
        return true;
    }
    if (!cJSON_IsNumber(v) || v->valueint < min || v->valueint > max) {
        snprintf(err, err_len, "%s は %d〜%d で指定してください", key, min, max);
        return false;
    }
    *out = v->valueint;
    return true;
}

bool config_from_json(const cJSON *o, char *err, size_t err_len)
{
    // 失敗したときに途中まで反映されないよう、コピーに対して適用する
    app_config_t c = g_cfg;
    int port = c.mqtt_port, pr = c.pin_reading, pb = c.pin_backlight, freq = c.pwm_freq;
    int sp = c.sled_pin == STATUS_LED_DISABLED ? -1 : c.sled_pin, sb = c.sled_brightness;

    if (!take_str(o, "wifi_ssid", c.wifi_ssid, sizeof(c.wifi_ssid), err, err_len) ||
        !take_str(o, "wifi_pass", c.wifi_pass, sizeof(c.wifi_pass), err, err_len) ||
        !take_str(o, "mqtt_host", c.mqtt_host, sizeof(c.mqtt_host), err, err_len) ||
        !take_str(o, "mqtt_user", c.mqtt_user, sizeof(c.mqtt_user), err, err_len) ||
        !take_str(o, "mqtt_pass", c.mqtt_pass, sizeof(c.mqtt_pass), err, err_len) ||
        !take_str(o, "device_name", c.device_name, sizeof(c.device_name), err, err_len) ||
        !take_str(o, "ts_authkey", c.ts_authkey, sizeof(c.ts_authkey), err, err_len) ||
        !take_str(o, "ts_hostname", c.ts_hostname, sizeof(c.ts_hostname), err, err_len) ||
        !take_int(o, "mqtt_port", 1, 65535, &port, err, err_len) ||
        !take_int(o, "pin_reading", 0, 48, &pr, err, err_len) ||
        !take_int(o, "pin_backlight", 0, 48, &pb, err, err_len) ||
        !take_int(o, "pwm_freq", 100, 20000, &freq, err, err_len) ||
        !take_int(o, "sled_pin", -1, 48, &sp, err, err_len) ||
        !take_int(o, "sled_brightness", 1, 255, &sb, err, err_len)) {
        return false;
    }
    if (!valid_output_pin(pr) || !valid_output_pin(pb)) {
        snprintf(err, err_len, "出力に使えない GPIO が指定されています");
        return false;
    }
    if (sp >= 0 && (!valid_output_pin(sp) || sp == pr || sp == pb)) {
        snprintf(err, err_len, "状態表示 LED に使えない GPIO が指定されています");
        return false;
    }
    if (pr == pb) {
        snprintf(err, err_len, "読書灯とバックライトに同じ GPIO は使えません");
        return false;
    }
    if (!c.device_name[0]) {
        snprintf(err, err_len, "device_name を入力してください");
        return false;
    }
    const char *bool_keys[] = {"pwm_invert", "ts_enabled", "sled_always_on", "log_verbose"};
    bool *bool_dst[] = {&c.pwm_invert, &c.ts_enabled, &c.sled_always_on, &c.log_verbose};
    for (int i = 0; i < 4; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, bool_keys[i]);
        if (!v) {
            continue;
        }
        if (!cJSON_IsBool(v)) {
            snprintf(err, err_len, "%s は true/false で指定してください", bool_keys[i]);
            return false;
        }
        *bool_dst[i] = cJSON_IsTrue(v);
    }
    if (!valid_hostname(c.ts_hostname)) {
        snprintf(err, err_len, "ts_hostname は英小文字・数字・- だけで入力してください");
        return false;
    }
    if (c.ts_authkey[0] && strncmp(c.ts_authkey, "tskey-", 6) != 0) {
        snprintf(err, err_len, "Tailscale の認証キーは tskey- で始まります");
        return false;
    }
    if (c.ts_enabled && !c.ts_authkey[0]) {
        snprintf(err, err_len, "Tailscale を使うには認証キーを入力してください");
        return false;
    }
    if (strcmp(c.ts_authkey, g_cfg.ts_authkey) != 0 || strcmp(c.ts_hostname, g_cfg.ts_hostname) != 0) {
        set_ts_reset_flag();  // 新しいキーで登録し直す
    }
    c.mqtt_port = port;
    c.pin_reading = pr;
    c.pin_backlight = pb;
    c.pwm_freq = freq;
    c.sled_pin = sp < 0 ? STATUS_LED_DISABLED : sp;
    c.sled_brightness = sb;
    g_cfg = c;
    return true;
}
