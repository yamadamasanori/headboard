#pragma once

#include <stdint.h>
#include "cJSON.h"
#include "leds.h"

// 長期の累計（起動・異常な再起動の回数、ライトごとの点灯時間・点灯回数・電力量）。
// NVS に保存し、再起動しても続きから数える（工場出荷時に戻すと 0 に戻る）
//
// 呼ぶ順番: nvs_flash_init() → leds_init() → usage_init()
void usage_init(void);

// ライトが消灯から点灯に変わったとき（leds.c から呼ぶ）
void usage_light_turned_on(light_id_t id);

// 意図して再起動する直前に呼ぶ（最後の保存からの分を書き込む）
void usage_flush(void);

// 累計を 0 に戻す（工場出荷時に戻すとき）
void usage_reset(void);

// Home Assistant 向けの値を o に追加する
//   boot_count, brownout_count, abnormal_reset_count, abnormal_reset（前回が異常か）,
//   lights.<key>.{on_time_h, on_count, power_w, energy_wh}
void usage_to_json(cJSON *o);
