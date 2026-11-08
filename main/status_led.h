#pragma once

#include <stdbool.h>
#include <stdint.h>

// 状態表示。Wi-Fi / Tailscale / MQTT の状態を、ボードに合わせて表示する
//   N16R8 : 基板上のフルカラー LED（WS2812）。status_led.c。
//           SSD1306 の OLED もつなげば（設定で SDA・SCL を指定）、両方に表示する
//   XIAO  : SSD1306 の OLED（128×64、I2C）。status_oled.c
//
// フルカラー LED の色と光り方
//   紫・ゆっくり点滅  Wi-Fi 未設定（Bluetooth での設定待ち）
//   青・速い点滅      Wi-Fi 接続中・切断
//   水色・点滅        Tailscale 接続中
//   黄・点滅          MQTT 接続待ち・切断（ブローカー未設定を含む）
//   赤・点滅          Tailscale の認証失敗、MQTT の接続拒否
//   緑・点灯          すべて正常（設定により 10 秒後に消灯）
// 問題が複数あるときは上にあるものを表示する（赤はその項目の色の代わりに出る）
// BOOT ボタンを押し続けている間は、離したときに行うことを示す
//   白・点灯          離すと Wi-Fi の設定を消して、Bluetooth での設定待ちにする（3 秒以上）
//   赤・点灯          離すとすべての設定を消す（10 秒以上）
void status_led_start(void);

// BOOT ボタンを押し続けている時間（離したら 0）。押している間の案内に使う
void status_led_button(uint32_t held_ms);

// BOOT ボタンを短く押したとき。消えている表示を、しばらく出し直す
void status_led_wake(void);

// Home Assistant からライトを操作したときに知らせる（light は light_id_t、分からなければ -1）
void status_led_pulse(int light);

// 本体の場所を知らせる（Home Assistant の「本体を光らせる」ボタン）
void status_led_identify(uint32_t ms);

// いま表示している状態の名前（"wifi_connecting"、"ok" など。設定ページの表示用）
const char *status_led_indicator(void);
