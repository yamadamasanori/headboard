#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "mqtt_client.h"

// MQTT ブローカーとの接続と、ライトの送受信（自動探索・状態・コマンド）。
// ライト以外の Home Assistant 連携は ha.c

#define HA_PREFIX "homeassistant"   // Home Assistant の自動探索のトピックの先頭

// トピックを決め、ライトが変わったら状態を送るようにする（net_start から呼ぶ）
void broker_init(void);
// Wi-Fi が繋がるたびに呼ぶ。ブローカーが LAN なら接続し、tailnet なら Tailscale が繋がってから接続する
void broker_on_wifi_up(void);

// 接続していなければ NULL
esp_mqtt_client_handle_t broker_client(void);
// headboard/<id>
const char *broker_base_topic(void);
// headboard/<id>/status（online / offline）
const char *broker_avail_topic(void);

bool broker_is_up(void);
// ブローカーに接続を拒否された（ユーザー名・パスワード違いなど）
bool broker_is_refused(void);
// 起動してから再接続した回数
uint32_t broker_reconnects(void);
// 設定ページ向けの状態（接続中か、接続先、繋がらない理由、トピック）
void broker_status_to_json(cJSON *obj);
