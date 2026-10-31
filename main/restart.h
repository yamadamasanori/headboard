#pragma once

#include <stdint.h>

// 意図した再起動。最後の保存からの累計（点灯時間など）を書き込んでから再起動する

// すぐに再起動する
void restart_now(void);
// delay_ms 後に再起動する。応答を返し終えてから再起動したいときに使う（呼んだタスクは止めない）
void restart_later(uint32_t delay_ms);
