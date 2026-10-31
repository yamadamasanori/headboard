#pragma once

#include <stddef.h>
#include <stdint.h>

// ログを本体のメモリにも貯めて、本体の設定ページから読めるようにする。
//   - 起動してからのログを PSRAM に貯める（古いものから消える）
//   - 再起動の直前のログとエラーを RTC メモリに残す（電源を切ると消える）
// 出力先（UART）はこれまでどおり
void logbuf_init(void);

// これまでに貯めたログの総バイト数（読み出し位置の基準）
uint64_t logbuf_end(void);

// *pos から最大 max バイトを dst に読み出し、*pos を進める。読んだバイト数を返す。
// *pos が古すぎて消えている場合は、残っている最も古い位置から読む
size_t logbuf_read(uint64_t *pos, char *dst, size_t max);

// 前回の起動の最後のログ（ソフトウェアによる再起動やクラッシュのときだけ。なければ空文字）
const char *logbuf_prev_tail(void);
// 前回の起動で最後に出たエラー（なければ空文字）
const char *logbuf_prev_error(void);
// 今回の起動で最後に出たエラー（なければ空文字）
const char *logbuf_last_error(void);

// 前回の再起動の理由（"電源投入"、"クラッシュ" など）
const char *logbuf_reset_reason(void);

// 前回クラッシュしたときの理由・タスク・Backtrace（フラッシュに記録があるときだけ。なければ空文字）
const char *logbuf_crash(void);
