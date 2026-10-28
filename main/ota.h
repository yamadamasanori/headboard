#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Wi-Fi 経由のファームウェア更新（本体の設定ページから）。
// 書き込みは使っていない方のアプリ領域に行い、起動に失敗したら前のファームウェアに戻る

// 更新後の初回起動で、正常に動いていることを記録する（記録しないまま再起動すると前に戻る）
void ota_mark_valid(void);

// 届いたファイルの先頭（最低 OTA_HEADER_LEN バイト）が、このプロジェクトの
// 更新用ファームウェア（headboard-led-ota.bin）かを調べる。違えば理由を err に入れて false
#define OTA_HEADER_LEN 288
bool ota_check_header(const uint8_t *buf, size_t len, char *err, size_t err_len);
