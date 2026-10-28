#pragma once

#include "cJSON.h"

#include <stdbool.h>

typedef struct {
    bool wifi_configured;
    bool wifi_up;
    bool mqtt_configured;
    bool mqtt_up;
    bool mqtt_refused;   // ブローカーに接続を拒否された（ユーザー名・パスワード違いなど）
    bool ble_setup;      // Bluetooth で Wi-Fi の設定を待っている
} net_state_t;

// Wi-Fi に接続し、接続後に MQTT（Home Assistant 連携）を開始する
void net_start(void);

// Wi-Fi / MQTT の接続状態を JSON に書き出す
void net_status_to_json(cJSON *obj);

// 状態表示 LED 用に、接続状態をまとめて返す
void net_get_state(net_state_t *st);
