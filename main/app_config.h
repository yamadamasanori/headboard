#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "cJSON.h"
#include "esp_err.h"

#define FW_VERSION "2.4.0"

#define STATUS_LED_DISABLED 0xFF
#define MQTT_CA_MAX 2560      // CA 証明書（PEM）の最大の長さ（終端を含む）。RSA 4096 bit の証明書まで入る

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char mqtt_host[64];
    uint16_t mqtt_port;
    char mqtt_user[64];
    char mqtt_pass[64];
    bool mqtt_tls;          // TLS で接続する（MQTTS、既定のポートは 8883）
    char mqtt_ca[MQTT_CA_MAX];  // ブローカーの証明書を確かめる CA 証明書（PEM）。空なら公開の認証局で確かめる
    char device_name[48];   // Home Assistant に表示されるデバイス名
    uint8_t pin_reading;    // 読書灯の GPIO
    uint8_t pin_backlight;  // バックライトの GPIO
    uint16_t pwm_freq;      // PWM 周波数 [Hz]
    bool pwm_invert;        // 出力を反転（アクティブLowのドライバ用）
    bool ts_enabled;        // Tailscale を使う
    bool tailnet_only;      // 本体の設定ページ（と更新）を tailnet からだけ受け付ける
    char ts_authkey[128];   // Tailscale の認証キー（tskey-auth-...）
    char ts_hostname[64];   // tailnet 上のホスト名
    uint8_t sled_pin;       // 状態表示 LED（WS2812）の GPIO。STATUS_LED_DISABLED で使わない
    uint8_t sled_brightness;// 状態表示 LED の明るさ 1〜255
    bool sled_always_on;    // 正常時も点灯し続ける（false なら 10 秒後に消灯）
    bool log_verbose;       // Tailscale などの詳細ログも出す（調査用）
    float watt_reading;     // 読書灯の定格消費電力 [W]（消費電力の推定用）
    float watt_backlight;   // バックライトの定格消費電力 [W]
} app_config_t;

// ログの出し方を設定に合わせる（起動時と設定変更時に呼ぶ）
void config_apply_log_levels(void);

extern app_config_t g_cfg;

// MAC アドレスから作るデバイス固有ID（例: headboard_a1b2c3）
const char *config_device_id(void);

// 本体の設定ページのホスト名（例: headboard-a1b2c3。".local" を付けて開く）
const char *config_hostname(void);

// Bluetooth での Wi-Fi 設定に使う名前（例: PROV_HB_a1b2c3）
const char *config_prov_name(void);

// 本体ごとのコード（PoP）。Bluetooth での Wi-Fi 設定と、本体の設定ページのログインに使う。
// チップ固有の ID から作るので、書き込み直しや初期化をしても変わらない（ラベルに印刷する）
const char *config_pop(void);

void config_load(void);
esp_err_t config_save(void);
esp_err_t config_factory_reset(void);
// Wi-Fi の設定だけを消す（Bluetooth で設定し直すとき）
esp_err_t config_clear_wifi(void);

// 認証キーが変わったとき、次回起動時に Tailscale の鍵を作り直すための印
bool config_take_ts_reset_flag(void);

// パスワード類は値を返さず、設定済みかどうかだけを返す
void config_to_json(cJSON *obj);

// JSON に含まれる項目だけを g_cfg に反映する。エラー時は false と理由を返す
bool config_from_json(const cJSON *obj, char *err, size_t err_len);
