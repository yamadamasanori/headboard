#pragma once

#include <stdbool.h>

// Bluetooth（BLE）で Wi-Fi を設定する。Wi-Fi が未設定のときに net.c から呼ぶ。
// スマホの公式アプリ「ESP BLE Provisioning」で、本体ごとのコード（PoP）を使って設定する。
// 繋がったら設定を保存し、Bluetooth を止める
void prov_start(void);

// Bluetooth で設定を待っている
bool prov_is_active(void);
