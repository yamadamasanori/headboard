#pragma once

// 本体の BOOT ボタン（GPIO0）
//   短く押す            : 状態表示を出し直す（消えているとき）
//   3 秒以上押して離す  : Wi-Fi の設定だけを消して再起動（Bluetooth での設定待ちになる）
//   10 秒以上押して離す : すべての設定を消して再起動（工場出荷時の状態）
// 押している間は、状態表示で離したときに行うことを知らせる
#define BUTTON_WIFI_RESET_MS    3000
#define BUTTON_FACTORY_RESET_MS 10000

void button_start(void);
