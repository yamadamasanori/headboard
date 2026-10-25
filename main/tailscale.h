#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

// Tailscale（MicroLink）で tailnet に接続する。
// Wi-Fi が繋がるたびに呼ぶ（2 回目以降は接続し直すだけ）。
// tailnet に繋がったら on_connected が呼ばれる。
typedef void (*ts_connected_cb_t)(void);
void ts_on_wifi_up(ts_connected_cb_t on_connected);

bool ts_is_connected(void);
// 認証失敗などで止まっている
bool ts_has_error(void);

// tailnet 上のホスト名（例: "homeassistant"）を VPN の IP アドレス文字列に変換する。
// 見つからなければ false
bool ts_resolve(const char *hostname, char *ip_out, size_t len);

// 保存されている Tailscale の鍵とピア情報を消す（再起動前に呼ぶ）
void ts_factory_reset(void);

void ts_status_to_json(cJSON *obj);
