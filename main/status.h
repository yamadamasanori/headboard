#pragma once

// 状態表示の共通部分（status.c）。表示する状態の判定と、ボタン・操作の記録を持ち、
// ボードごとの表示（status_led.c / status_oled.c）から読む

#include <stdbool.h>
#include <stdint.h>
#include "status_led.h"

// 表示する状態。問題が複数あるときは、上にあるものを返す
typedef enum {
    ST_WIFI_UNCONFIGURED,   // Wi-Fi 未設定（Bluetooth での設定を始めていない）
    ST_BLE_SETUP,           // Bluetooth での Wi-Fi の設定待ち
    ST_WIFI_CONNECTING,     // Wi-Fi に接続中・切断
    ST_TS_AUTH_ERROR,       // Tailscale の認証失敗
    ST_TS_CONNECTING,       // Tailscale に接続中
    ST_MQTT_REFUSED,        // ブローカーに接続を拒否された
    ST_MQTT_UNCONFIGURED,   // ブローカーが未設定
    ST_MQTT_CONNECTING,     // ブローカーに接続中・切断
    ST_OK,                  // すべて正常
} status_t;

// BOOT ボタンを押し続けているときの案内
typedef enum {
    STATUS_HINT_NONE,
    STATUS_HINT_WIFI_RESET,      // 離すと Wi-Fi の設定を消す
    STATUS_HINT_FACTORY_RESET,   // 離すとすべての設定を消す
} status_hint_t;

// 今の状態を決める（設定ページに出す名前も更新する）
status_t status_decide(void);

uint32_t status_button_held_ms(void);
status_hint_t status_hint(void);

// 最後に操作・ボタンを短く押したときの時刻（esp_timer_get_time()、まだなければ 0）
int64_t status_pulse_at_us(void);
int status_pulse_light(void);
int64_t status_wake_at_us(void);
// 本体を光らせる（場所を知らせる）のを終える時刻
int64_t status_identify_until_us(void);

// 表示ごとの開始と、使えるか（使わない設定・初期化に失敗したときは false）
//   status_rgb_*  : 基板上のフルカラー LED（status_led.c。N16R8 だけ）
//   status_oled_* : SSD1306 の OLED（status_oled.c。どのボードでも）
void status_rgb_start(void);
bool status_rgb_enabled(void);
void status_oled_start(void);
bool status_oled_enabled(void);
