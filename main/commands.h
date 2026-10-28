#pragma once

#include "cJSON.h"

// 設定ページからのコマンド（1 行 1 コマンドの JSON。例: {"cmd":"get"}）を処理する。
// USB（console.c）と本体の設定ページ（webui.c）で共通
typedef enum {
    CMD_SRC_USB,   // USB 接続。量産版（CONFIG_HEADBOARD_USB_SETUP=n）では設定の変更を受け付けない
    CMD_SRC_WEB,   // 本体の設定ページ（ログイン済み）
} cmd_src_t;

// 応答の送り先。json は "@@" を付ける前の 1 行の JSON
typedef void (*cmd_send_fn_t)(const char *json, void *ctx);

void cmd_handle(const char *line, cmd_src_t src, cmd_send_fn_t send, void *ctx);

// hello の応答を作る（呼び出し側で cJSON_Delete）
cJSON *cmd_hello(cmd_src_t src);
