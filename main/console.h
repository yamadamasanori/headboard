#pragma once

// USB シリアル経由のコマンドを受け付ける（処理は commands.c）。
// 1 行 1 コマンドの JSON（例: {"cmd":"get"}）を受け取り、
// 応答は "@@" に続く 1 行の JSON で返す（ログ出力と区別するため）。
void console_start(void);
