#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

typedef enum {
    LIGHT_READING = 0,   // 読書灯
    LIGHT_BACKLIGHT,     // バックライト
    LIGHT_COUNT,
} light_id_t;

// MQTT トピックや設定画面で使う識別子（"reading" / "backlight"）
const char *leds_key(light_id_t id);
// Home Assistant に表示する名前
const char *leds_display_name(light_id_t id);
// "reading" などから ID を引く。該当なしは -1
int leds_find(const char *key);

void leds_init(void);

// 状態が変わったときに呼ばれる（MQTT への状態送信用）
typedef void (*leds_changed_cb_t)(light_id_t id);
void leds_set_changed_cb(leds_changed_cb_t cb);

// Home Assistant の JSON スキーマ形式のコマンドを適用する
// 例: {"state":"ON","brightness":128,"transition":2}
// "ON" / "OFF" だけの文字列ペイロードも受け付ける
bool leds_apply_command(light_id_t id, const char *payload, int len);

// 直接設定（brightness 0〜255、0 で消灯）
void leds_set(light_id_t id, bool on, int brightness, uint32_t transition_ms);

// 現在の状態を Home Assistant 形式の JSON で返す（呼び出し側で free）
char *leds_state_json(light_id_t id);
void leds_state_to_json(light_id_t id, cJSON *obj);
