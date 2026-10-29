#pragma once

#include <stdbool.h>
#include "cJSON.h"
#include "mqtt_client.h"

// Home Assistant 連携のうち、ライト以外のもの（ライトは net.c）
//   - 診断: 電波の強さ・IP・稼働時間・再起動の理由・エラー・メモリ・温度・Tailscale など
//   - 累計: 起動・電圧低下の回数、ライトごとの点灯時間・点灯回数・消費電力・電力量
//   - 操作: 再起動、本体を光らせる、状態表示 LED の明るさ・常時点灯、詳細ログ
//   - 更新: 配布元（CONFIG_HEADBOARD_UPDATE_URL）の新しい版を Home Assistant から書き込む
// 値は <base>/diag に 60 秒ごと（と変化したとき）にまとめて送る
void ha_init(void);

// MQTT に繋がったとき・Home Assistant が起動し直したとき（自動探索の設定を送る）
void ha_on_mqtt_connected(esp_mqtt_client_handle_t client, const char *base, const char *avail_topic);
void ha_on_mqtt_disconnected(void);

// 受信したメッセージがこのモジュール宛てなら処理して true を返す
bool ha_handle_message(const char *topic, const char *data, int len);

// 自動探索の設定に、デバイスの情報（"device"）を追加する
void ha_add_device(cJSON *config);
