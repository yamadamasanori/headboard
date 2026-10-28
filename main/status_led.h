#pragma once

// オンボードのフルカラー LED（WS2812）で、Wi-Fi / Tailscale / MQTT の状態を表示する
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

typedef enum {
    STATUS_HINT_NONE,
    STATUS_HINT_WIFI_RESET,
    STATUS_HINT_FACTORY_RESET,
} status_hint_t;
void status_led_set_hint(status_hint_t hint);

// Home Assistant からの操作を受けたときに、緑を一瞬光らせる
void status_led_pulse(void);

// いま表示している状態の名前（"wifi_connecting"、"ok" など。設定ページの表示用）
const char *status_led_indicator(void);
