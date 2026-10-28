#pragma once

// 本体の設定ページ（http://headboard-xxxxxx.local/）。Wi-Fi に繋がったら net.c から呼ぶ。
//   - 設定ページ（web/index.html を組み込んだもの）を配信する
//   - /ws : WebSocket。USB と同じ 1 行 1 コマンドの JSON と、ログを流す
//           最初に {"cmd":"login","pop":"<本体のコード>"} でログインする
//   - /ota: 更新用ファームウェア（headboard-led-ota.bin）の書き込み
void webui_start(void);
