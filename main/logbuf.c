#include "logbuf.h"

#include <stdarg.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define RING_SIZE   (64 * 1024)   // 起動してからのログ（PSRAM）
#define TAIL_SIZE   2048          // 再起動の直前のログ（RTC メモリ）
#define ERR_SIZE    192
#define CRASH_SIZE  512
#define LOG_LINE_MAX    256
#define RTC_MAGIC   0x4c4f4731u   // "LOG1"

// RTC メモリはソフトウェアによる再起動やクラッシュでは消えない
typedef struct {
    uint32_t magic;
    uint32_t pos;                 // tail の次の書き込み位置（総バイト数）
    char tail[TAIL_SIZE];
    char err[ERR_SIZE];
} rtc_log_t;

static RTC_NOINIT_ATTR rtc_log_t s_rtc;

static char *s_ring;
static uint64_t s_total;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig;
// 整形用のバッファはタスクのスタックに置かず、1 つを順番に使う（スタックの小さいタスクでも溢れないように）
static SemaphoreHandle_t s_fmt_lock;
static char s_line[LOG_LINE_MAX];

static char s_prev_tail[TAIL_SIZE + 1];
static char s_prev_err[ERR_SIZE];
static char s_last_err[ERR_SIZE];
static const char *s_reset_reason = "";
static char s_crash[CRASH_SIZE];

// 色付けのエスケープを除いた行の先頭が "E (" ならエラー
static const char *strip_color(const char *s)
{
    while (s[0] == '\033' && s[1] == '[') {
        const char *m = strchr(s, 'm');
        if (!m) {
            break;
        }
        s = m + 1;
    }
    return s;
}

static void copy_line(char *dst, size_t size, const char *line)
{
    size_t n = strcspn(line, "\r\n\033");
    if (n >= size) {
        n = size - 1;
    }
    memcpy(dst, line, n);
    dst[n] = 0;
}

static void append(const char *buf, size_t len)
{
    const char *plain = strip_color(buf);
    bool is_error = plain[0] == 'E' && plain[1] == ' ' && plain[2] == '(';

    taskENTER_CRITICAL(&s_mux);
    if (s_ring) {
        for (size_t i = 0; i < len;) {
            size_t at = s_total % RING_SIZE;
            size_t n = len - i < RING_SIZE - at ? len - i : RING_SIZE - at;
            memcpy(s_ring + at, buf + i, n);
            s_total += n;
            i += n;
        }
    }
    for (size_t i = 0; i < len; i++) {
        s_rtc.tail[s_rtc.pos % TAIL_SIZE] = buf[i];
        s_rtc.pos++;
    }
    if (is_error) {
        copy_line(s_last_err, sizeof(s_last_err), plain);
        memcpy(s_rtc.err, s_last_err, sizeof(s_rtc.err));
    }
    taskEXIT_CRITICAL(&s_mux);
}

static int hook(const char *fmt, va_list ap)
{
    // 割り込みの中やスケジューラが止まっているときは、貯めずに出力だけする
    if (xPortInIsrContext() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||
        xSemaphoreTake(s_fmt_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return s_orig(fmt, ap);
    }
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(s_line, sizeof(s_line), fmt, ap2);
    va_end(ap2);
    int r = s_orig(fmt, ap);
    if (n > 0) {
        size_t len = (size_t)n;
        if (len >= sizeof(s_line)) {  // 長すぎる行は切り詰める
            len = sizeof(s_line) - 1;
            s_line[len - 1] = '\n';
        }
        append(s_line, len);
    }
    xSemaphoreGive(s_fmt_lock);
    return r;
}

static const char *reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "電源投入";
    case ESP_RST_EXT: return "外部リセット";
    case ESP_RST_SW: return "ソフトウェアによる再起動";
    case ESP_RST_PANIC: return "クラッシュ（例外）";
    case ESP_RST_INT_WDT: return "割り込みウォッチドッグ";
    case ESP_RST_TASK_WDT: return "タスクウォッチドッグ";
    case ESP_RST_WDT: return "ウォッチドッグ";
    case ESP_RST_DEEPSLEEP: return "スリープからの復帰";
    case ESP_RST_BROWNOUT: return "電源電圧の低下";
    case ESP_RST_USB: return "USB からのリセット";
    case ESP_RST_JTAG: return "JTAG からのリセット";
    default: return "不明";
    }
}

// 前回クラッシュしたときにフラッシュに残した記録（core dump）から、理由・タスク・Backtrace を取り出す。
// シリアルをつないでいなくても、クラッシュした場所を本体の設定ページで見られるようにする
static void read_crash(void)
{
    if (esp_core_dump_image_check() != ESP_OK) {
        return;
    }
    esp_core_dump_summary_t *sum = calloc(1, sizeof(*sum));
    if (sum && esp_core_dump_get_summary(sum) == ESP_OK) {
        char reason[160] = "";
        if (esp_core_dump_get_panic_reason(reason, sizeof(reason)) != ESP_OK) {
            snprintf(reason, sizeof(reason), "例外 %" PRIu32, sum->ex_info.exc_cause);
        }
        reason[strcspn(reason, "\r\n")] = 0;
        int n = snprintf(s_crash, sizeof(s_crash), "%s（タスク %s、PC 0x%08" PRIx32 "）Backtrace:",
                         reason, sum->exc_task, sum->exc_pc);
        for (uint32_t i = 0; i < sum->exc_bt_info.depth && n > 0 && n < (int)sizeof(s_crash); i++) {
            n += snprintf(s_crash + n, sizeof(s_crash) - n, " 0x%08" PRIx32, sum->exc_bt_info.bt[i]);
        }
        if (sum->exc_bt_info.corrupted && n > 0 && n < (int)sizeof(s_crash)) {
            snprintf(s_crash + n, sizeof(s_crash) - n, " |<-CORRUPTED");
        }
    }
    free(sum);
    esp_core_dump_image_erase();  // 同じクラッシュを次の起動でも出さないように消す
}

void logbuf_init(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    s_reset_reason = reason_name(r);
    read_crash();

    // 電源投入時の RTC メモリは不定なので使わない
    if (r != ESP_RST_POWERON && r != ESP_RST_BROWNOUT && s_rtc.magic == RTC_MAGIC) {
        uint32_t n = s_rtc.pos < TAIL_SIZE ? s_rtc.pos : TAIL_SIZE;
        uint32_t start = s_rtc.pos - n;
        for (uint32_t i = 0; i < n; i++) {
            s_prev_tail[i] = s_rtc.tail[(start + i) % TAIL_SIZE];
        }
        s_prev_tail[n] = 0;
        // 途中で切れた先頭の行は捨てる
        char *nl = n == TAIL_SIZE ? strchr(s_prev_tail, '\n') : NULL;
        if (nl) {
            memmove(s_prev_tail, nl + 1, strlen(nl + 1) + 1);
        }
        s_rtc.err[ERR_SIZE - 1] = 0;
        strlcpy(s_prev_err, s_rtc.err, sizeof(s_prev_err));
    }
    memset(&s_rtc, 0, sizeof(s_rtc));
    s_rtc.magic = RTC_MAGIC;

    s_ring = heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) {
        s_ring = heap_caps_malloc(RING_SIZE / 4, MALLOC_CAP_8BIT);  // PSRAM がないときは小さく
    }
    s_fmt_lock = xSemaphoreCreateMutex();
    s_orig = esp_log_set_vprintf(hook);
}

uint64_t logbuf_end(void)
{
    taskENTER_CRITICAL(&s_mux);
    uint64_t t = s_total;
    taskEXIT_CRITICAL(&s_mux);
    return t;
}

size_t logbuf_read(uint64_t *pos, char *dst, size_t max)
{
    if (!s_ring) {
        return 0;
    }
    taskENTER_CRITICAL(&s_mux);
    uint64_t oldest = s_total > RING_SIZE ? s_total - RING_SIZE : 0;
    if (*pos < oldest) {
        *pos = oldest;
    }
    if (*pos > s_total) {
        *pos = s_total;
    }
    size_t n = s_total - *pos < max ? (size_t)(s_total - *pos) : max;
    for (size_t i = 0; i < n;) {
        size_t at = (*pos + i) % RING_SIZE;
        size_t k = n - i < RING_SIZE - at ? n - i : RING_SIZE - at;
        memcpy(dst + i, s_ring + at, k);
        i += k;
    }
    *pos += n;
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

const char *logbuf_prev_tail(void) { return s_prev_tail; }
const char *logbuf_prev_error(void) { return s_prev_err; }
const char *logbuf_last_error(void) { return s_last_err; }
const char *logbuf_reset_reason(void) { return s_reset_reason; }
const char *logbuf_crash(void) { return s_crash; }
