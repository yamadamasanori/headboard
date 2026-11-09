// 状態表示（XIAO ESP32S3 は標準、N16R8 は設定したときだけ）: SSD1306 の OLED（128×64、I2C）に、Wi-Fi / Tailscale / MQTT の状態を表示する
//
//   ┌──────────────────────┐
//   │headboard-a1b2c3 ▂▄▆ TS MQ│ 上段 16px: ホスト名と接続状態のアイコン（5×8 のフォント）
//   ├──────────────────────┤
//   │ 本文 3 行（1 行 16 文字）│ 下段 48px: 状態ごとの表示（8×13 太字のフォント）
//   └──────────────────────┘
// アイコンは、繋がっていれば塗りつぶし、接続中は枠だけ、エラーは斜線入り
//
// 0.96 インチの 2 色（上 16px が黄、下が青）のモジュールでも区切りが合うように、上段は 16px にする。
// 焼き付きを防ぐため、正常になってから 10 秒で画面を消す（「常に表示」の設定がなければ）。
// 長く表示し続ける画面は、1 分ごとに 0〜2 ドット横にずらす

#include "status.h"

#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "board.h"
#include "button.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "leds.h"
#include "net.h"
#include "oled_font.h"
#include "tailscale.h"

static const char *TAG = "status_oled";

#define W               128
#define H               64
#define RIGHT           (W - 2)  // 右端（ここより左に描く）。ずらしても欠けないように 2 ドット空ける
#define HEADER_H        16
#define LINE_CHARS      16
#define TICK_MS         50
#define BOOT_MS         1000     // 起動時の表示
#define OK_HOLD_MS      10000    // 正常になってから画面を消すまで
#define WAKE_MS         10000    // BOOT ボタンを短く押したときに表示する時間
#define PULSE_SHOW_MS   5000     // Home Assistant からライトを操作したときに表示する時間
#define PULSE_MARK_MS   1000     // 操作したライトの行を反転する時間
#define BUTTON_SHOW_MS  500      // これより短い押し方では案内を出さない（短く押して表示を出し直すとき）
#define IP_SWAP_MS      3000     // LAN と tailnet の IP アドレスを入れ替える間隔
#define SHIFT_MS        60000    // 焼き付き対策で表示をずらす間隔
#define I2C_HZ          400000
#define I2C_TIMEOUT_MS  100
#define POWERUP_MS      500      // 電源を入れてから OLED が応えるようになるまで待つ時間
#define RETRY_MS        2000     // 見つからない・書き込みに失敗したときに、探し直す間隔

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static volatile bool s_ready;  // OLED が見つかって、初期化できている
static uint8_t s_fb[W * H / 8];       // 1 バイトが縦 8 ドット（SSD1306 のページ形式）
static uint8_t s_tx[1 + sizeof(s_fb)];  // 送信用。先頭は制御バイト 0x40（データ）
static int s_shift;

// ---------------------------------------------------------------- SSD1306

static esp_err_t send_cmds(const uint8_t *cmds, size_t n)
{
    uint8_t buf[8];
    if (n > sizeof(buf) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = 0x00;  // 制御バイト: 続きはコマンド
    memcpy(buf + 1, cmds, n);
    return i2c_master_transmit(s_dev, buf, n + 1, I2C_TIMEOUT_MS);
}

#define CMD(...) send_cmds((const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))

static esp_err_t send_frame(void)
{
    esp_err_t err = CMD(0x21, 0, W - 1);  // 列の範囲
    if (err == ESP_OK) {
        err = CMD(0x22, 0, H / 8 - 1);  // ページの範囲
    }
    if (err == ESP_OK) {
        s_tx[0] = 0x40;
        memcpy(s_tx + 1, s_fb, sizeof(s_fb));
        err = i2c_master_transmit(s_dev, s_tx, sizeof(s_tx), I2C_TIMEOUT_MS);
    }
    return err;
}

// 明るさ 1〜255 を、OLED の設定に変える。
//   32〜255: コントラストだけを変える（見た目の変化がおおよそ均等になるよう 2 乗で。255 → 255、128 → 65、32 → 5）
//   1〜31  : コントラストは 5 以下ではほとんど変わらないので、プリチャージ期間と VCOMH の電圧を段階的に下げる。
//            実機では、0x11・0x00（コントラスト 1）で何も見えなくなったので、そこまでは下げない
static const struct {
    uint8_t contrast, precharge, vcomh;
} DIM_STEPS[8] = {
    // 明るさ 1〜3, 4〜7, 8〜11, …, 28〜31 の順
    {1, 0x21, 0x00},
    {1, 0x31, 0x00},
    {1, 0x51, 0x00},
    {1, 0x71, 0x00},
    {2, 0xA1, 0x20},
    {3, 0xC1, 0x20},
    {4, 0xF1, 0x20},
    {4, 0xF1, 0x30},
};

static esp_err_t set_brightness(uint8_t b)
{
    uint8_t contrast, precharge = 0xF1, vcomh = 0x40;  // 内部の昇圧回路を使うときの標準のプリチャージと VCOMH
    if (b >= 32) {
        contrast = (uint8_t)(((unsigned)b * b + 254) / 255);
    } else {
        contrast = DIM_STEPS[b / 4].contrast;
        precharge = DIM_STEPS[b / 4].precharge;
        vcomh = DIM_STEPS[b / 4].vcomh;
    }
    esp_err_t err = CMD(0x81, contrast);
    if (err == ESP_OK) {
        err = CMD(0xD9, precharge);  // 上位 4 ビットが第 2 期間、下位 4 ビットが第 1 期間（クロック数）
    }
    if (err == ESP_OK) {
        err = CMD(0xDB, vcomh);  // 0x00 は約 0.65 Vcc、0x20 は約 0.77 Vcc、0x30 は約 0.83 Vcc
    }
    return err;
}

// 180° 回すときは、列と行の走査の向きを両方逆にする
static esp_err_t set_flip(bool flip)
{
    esp_err_t err = CMD(flip ? 0xA0 : 0xA1);
    if (err == ESP_OK) {
        err = CMD(flip ? 0xC0 : 0xC8);
    }
    return err;
}

static esp_err_t set_power(bool on)
{
    return CMD(on ? 0xAF : 0xAE);
}

static esp_err_t oled_init(bool flip, uint8_t brightness)
{
    static const uint8_t seq[][3] = {
        {2, 0xD5, 0x80},  // 表示クロック
        {2, 0xA8, 0x3F},  // 64 行
        {2, 0xD3, 0x00},  // 表示のずれなし
        {1, 0x40},        // 開始行 0
        {2, 0x8D, 0x14},  // 内部の昇圧回路を使う
        {2, 0x20, 0x00},  // 横方向にアドレスを進める
        {2, 0xDA, 0x12},  // COM ピンの配置（128×64）
        {1, 0xA4},        // 画面メモリの内容を表示
        {1, 0xA6},        // 白黒を反転しない
        {1, 0x2E},        // スクロールしない
    };
    esp_err_t err = set_power(false);
    for (size_t i = 0; err == ESP_OK && i < sizeof(seq) / sizeof(seq[0]); i++) {
        err = send_cmds(&seq[i][1], seq[i][0]);
    }
    if (err == ESP_OK) {
        err = set_flip(flip);
    }
    if (err == ESP_OK) {
        err = set_brightness(brightness);
    }
    if (err == ESP_OK) {
        memset(s_fb, 0, sizeof(s_fb));
        err = send_frame();
    }
    return err;
}

// ---------------------------------------------------------------- 描画

static void px(int x, int y, bool on)
{
    x += s_shift;
    if (x < 0 || x >= W || y < 0 || y >= H) {
        return;
    }
    uint8_t *b = &s_fb[(y / 8) * W + x];
    uint8_t m = 1 << (y & 7);
    *b = on ? (*b | m) : (*b & ~m);
}

static void fill(int x, int y, int w, int h, bool on)
{
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            px(i, j, on);
        }
    }
}

static void frame(int x, int y, int w, int h, bool on)
{
    fill(x, y, w, 1, on);
    fill(x, y + h - 1, w, 1, on);
    fill(x, y, 1, h, on);
    fill(x + w - 1, y, 1, h, on);
}

static void line(int x0, int y0, int x1, int y1, bool on)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int e = dx + dy;
    for (;;) {
        px(x0, y0, on);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * e;
        if (e2 >= dy) {
            e += dy, x0 += sx;
        }
        if (e2 <= dx) {
            e += dx, y0 += sy;
        }
    }
}

// 文字を描く（文字の部分だけを on の色で塗る）。scale 倍に拡大できる。描いた右端の x を返す
static int text(const oled_font_t *f, int x, int y, const char *s, bool on, int scale)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < OLED_FONT_FIRST || c > OLED_FONT_LAST) {
            c = '?';
        }
        const uint8_t *g = f->data + (c - OLED_FONT_FIRST) * f->h;
        for (int r = 0; r < f->h; r++) {
            for (int col = 0; col < f->w; col++) {
                if (g[r] & (0x80 >> col)) {
                    fill(x + col * scale, y + r * scale, scale, scale, on);
                }
            }
        }
        x += f->w * scale;
    }
    return x;
}

static int text_width(const oled_font_t *f, const char *s, int scale)
{
    return (int)strlen(s) * f->w * scale;
}

static int body_y(int row)
{
    return HEADER_H + row * 16 + 2;  // 16px の行の中に 13px の文字を置く
}

// 本文の行（16 文字で切る）
static void body(int row, const char *s, bool on)
{
    char buf[LINE_CHARS + 1];
    strlcpy(buf, s, sizeof(buf));
    text(&oled_font_large, 0, body_y(row), buf, on, 1);
}

static void body_right(int row, const char *s, bool on)
{
    text(&oled_font_large, RIGHT - text_width(&oled_font_large, s, 1), body_y(row), s, on, 1);
}

static void center(int y, const char *s, bool on, int scale)
{
    char buf[LINE_CHARS + 1];
    strlcpy(buf, s, sizeof(buf));
    text(&oled_font_large, (W - text_width(&oled_font_large, buf, scale)) / 2, y, buf, on, scale);
}

static bool blink(int64_t now_ms, int period_ms)
{
    return (now_ms % period_ms) < period_ms / 2;
}

// ---------------------------------------------------------------- 画面

typedef enum {
    ICON_HIDDEN,
    ICON_OK,          // 繋がっている: 塗りつぶし
    ICON_CONNECTING,  // 接続中: 枠だけ
    ICON_ERROR,       // エラー: 枠と斜線
} icon_t;

// 上段の文字アイコン（TS、MQ）。幅 13、高さ 12
static void label_icon(int x, const char *label, icon_t st)
{
    const int y = 2, w = 13, h = 12;
    switch (st) {
    case ICON_HIDDEN:
        return;
    case ICON_OK:
        fill(x, y, w, h, true);
        text(&oled_font_small, x + 2, y + 2, label, false, 1);
        return;
    case ICON_CONNECTING:
        frame(x, y, w, h, true);
        text(&oled_font_small, x + 2, y + 2, label, true, 1);
        return;
    case ICON_ERROR:
        frame(x, y, w, h, true);
        text(&oled_font_small, x + 2, y + 2, label, true, 1);
        line(x, y + h - 1, x + w - 1, y, true);
        return;
    }
}

// Wi-Fi の電波の強さ（4 本の棒）。x から幅 11
static void wifi_icon(int x, const status_t st, int rssi, int64_t now_ms)
{
    if (st == ST_WIFI_UNCONFIGURED || st == ST_BLE_SETUP) {
        line(x + 2, 4, x + 8, 10, true);  // ×
        line(x + 2, 10, x + 8, 4, true);
        return;
    }
    int level = 0;
    if (st != ST_WIFI_CONNECTING) {
        level = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
    } else if (!blink(now_ms, 1000)) {
        return;  // 接続中は点滅
    }
    for (int i = 0; i < 4; i++) {
        int h = 3 + i * 2;
        if (st == ST_WIFI_CONNECTING) {
            frame(x + i * 3, 13 - h, 2, h, true);
        } else if (i < level) {
            fill(x + i * 3, 13 - h, 2, h, true);
        } else {
            fill(x + i * 3, 12, 2, 1, true);  // 届いていない分は下の 1 ドットだけ
        }
    }
}

static void draw_header(status_t st, int rssi, bool ts_ip_shown, int64_t now_ms)
{
    char host[17];  // headboard-xxxxxx（16 文字、80 ドット）
    strlcpy(host, config_hostname(), sizeof(host));
    text(&oled_font_small, 0, 4, host, true, 1);
    wifi_icon(86, st, rssi, now_ms);

    bool wifi_up = st > ST_WIFI_CONNECTING;
    icon_t ts = !g_cfg.ts_enabled           ? ICON_HIDDEN
                : st == ST_TS_AUTH_ERROR    ? ICON_ERROR
                : wifi_up && st != ST_TS_CONNECTING ? ICON_OK
                                            : ICON_CONNECTING;
    icon_t mq = st == ST_MQTT_REFUSED ? ICON_ERROR : st == ST_OK ? ICON_OK : ICON_CONNECTING;
    label_icon(99, "TS", ts);
    label_icon(113, "MQ", mq);
    if (ts_ip_shown) {
        fill(99, 15, 13, 1, true);  // 下段の IP アドレスが tailnet のものであることを示す下線
    }
}

static void draw_lights(int64_t now_ms)
{
    static const char *names[LIGHT_COUNT] = {"Reading", "Backlight"};
    int64_t pulse_ms = status_pulse_at_us() / 1000;
    int marked = pulse_ms && now_ms - pulse_ms < PULSE_MARK_MS ? status_pulse_light() : -1;
    for (int i = 0; i < LIGHT_COUNT; i++) {
        bool on;
        uint8_t bri;
        leds_get(i, &on, &bri);
        bool fg = i != marked;
        if (!fg) {
            fill(0, HEADER_H + i * 16, RIGHT, 16, true);  // 操作した行を反転する
        }
        text(&oled_font_large, 0, body_y(i), names[i], fg, 1);
        text(&oled_font_large, 76, body_y(i), on ? "ON" : "OFF", fg, 1);
        if (on) {
            char pct[6];
            snprintf(pct, sizeof(pct), "%d%%", (bri * 100 + 127) / 255);
            body_right(i, pct, fg);
        }
    }
}

static void draw_status(status_t st, const char *ip, int64_t now_ms)
{
    char ts_ip[16];
    switch (st) {
    case ST_WIFI_UNCONFIGURED:
        body(0, "Wi-Fi not set", true);
        body(1, "Starting BLE...", true);
        break;
    case ST_BLE_SETUP:
        body(0, "Wi-Fi not set", true);
        body(1, "Setup via BLE:", true);
        body(2, config_prov_name(), true);  // 設定用コード（PoP）は秘密なので出さない
        break;
    case ST_WIFI_CONNECTING:
        body(0, "Wi-Fi", true);
        body(1, "Connecting...", true);
        body(2, g_cfg.wifi_ssid, true);
        break;
    case ST_TS_AUTH_ERROR:
        body(0, "Tailscale", true);
        body(1, "Auth failed", true);
        body(2, "Check auth key", true);
        break;
    case ST_TS_CONNECTING:
        body(0, "Tailscale", true);
        body(1, "Connecting...", true);
        break;
    case ST_MQTT_REFUSED:
        body(0, "MQTT", true);
        body(1, "Refused", true);
        body(2, "Check user/pass", true);
        break;
    case ST_MQTT_UNCONFIGURED:
        body(0, "MQTT", true);
        body(1, "Not configured", true);
        body(2, "Open web setup", true);
        break;
    case ST_MQTT_CONNECTING:
        body(0, "MQTT", true);
        body(1, "Connecting...", true);
        body(2, g_cfg.mqtt_host, true);
        break;
    case ST_OK:
        draw_lights(now_ms);
        // Tailscale を使っていれば、LAN と tailnet の IP アドレスを交互に出す
        ts_vpn_ip_str(ts_ip);
        bool show_ts = g_cfg.ts_enabled && ts_ip[0] && (now_ms / IP_SWAP_MS) % 2 == 1;
        body(2, show_ts ? ts_ip : ip, true);
        break;
    }
    if ((st == ST_TS_AUTH_ERROR || st == ST_MQTT_REFUSED) && blink(now_ms, 1000)) {
        body_right(1, "(!)", true);
    }
}

static bool ts_ip_shown(status_t st, int64_t now_ms)
{
    if (st != ST_OK || !g_cfg.ts_enabled) {
        return false;
    }
    char ts_ip[16];
    ts_vpn_ip_str(ts_ip);
    return ts_ip[0] && (now_ms / IP_SWAP_MS) % 2 == 1;
}

// BOOT ボタンを押している間の案内
static void draw_button(uint32_t held_ms)
{
    if (held_ms >= BUTTON_FACTORY_RESET_MS) {
        fill(0, 0, W, H, true);  // 全体を反転して強調する
        body(0, "Release to:", false);
        body(1, "RESET ALL", false);
        body(2, "Erase all config", false);
        return;
    }
    if (held_ms < BUTTON_WIFI_RESET_MS) {
        body(0, "3s: Reset Wi-Fi", true);
        body(1, "10s: Reset all", true);
    } else {
        body(0, "Release to:", true);
        body(1, "Reset Wi-Fi", true);
    }
    // 10 秒までの進み具合（3 秒の位置に目盛り）
    const int bx = 0, by = body_y(2) + 1, bw = 96, bh = 10;
    frame(bx, by, bw, bh, true);
    fill(bx, by, (int)((bw - 1) * held_ms / BUTTON_FACTORY_RESET_MS) + 1, bh, true);
    int mark = bx + bw * BUTTON_WIFI_RESET_MS / BUTTON_FACTORY_RESET_MS;
    fill(mark, by - 2, 1, bh + 4, held_ms < BUTTON_WIFI_RESET_MS);
    char sec[8];
    snprintf(sec, sizeof(sec), "%us", (unsigned)(held_ms / 1000));
    body_right(2, sec, true);
}

static void draw_identify(int64_t now_ms)
{
    bool inv = blink(now_ms, 200);
    if (inv) {
        fill(0, 0, W, H, true);
    }
    center((H - oled_font_large.h * 2) / 2, "HERE", !inv, 2);
}

static void draw_boot(void)
{
    center(6, "Headboard LED", true, 1);
    center(25, "v" FW_VERSION, true, 1);
    center(44, BOARD_NAME, true, 1);
}

// ---------------------------------------------------------------- タスク

// OLED を探して初期化する。アドレスは 0x3C（基板の設定で 0x3D のものもある）
static esp_err_t oled_attach(bool flip, uint8_t brightness, bool log_missing)
{
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    uint16_t addr = 0;
    for (uint16_t a = 0x3C; a <= 0x3D && !addr; a++) {
        if (i2c_master_probe(s_bus, a, I2C_TIMEOUT_MS) == ESP_OK) {
            addr = a;
        }
    }
    if (!addr) {
        if (log_missing) {
            ESP_LOGE(TAG, "OLED not found on I2C (SDA GPIO%u, SCL GPIO%u). Check wiring. Retrying every %d s",
                     g_cfg.oled_sda, g_cfg.oled_scl, RETRY_MS / 1000);
        }
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(s_bus, &dev, &s_dev);
    if (err == ESP_OK) {
        err = oled_init(flip, brightness);
    }
    if (err != ESP_OK) {
        if (log_missing) {
            ESP_LOGE(TAG, "OLED init failed: %s. Retrying every %d s", esp_err_to_name(err), RETRY_MS / 1000);
        }
        return err;
    }
    ESP_LOGI(TAG, "OLED (SSD1306 at 0x%02x) on SDA GPIO%u, SCL GPIO%u", addr, g_cfg.oled_sda, g_cfg.oled_scl);
    return ESP_OK;
}

static void oled_task(void *arg)
{
    static uint8_t sent[sizeof(s_fb)];
    int64_t start_ms = 0, retry_at_ms = 0, ok_since_ms = -1, last_link_ms = -1000000;
    char ip[16] = "";
    int rssi = 0;
    bool power = false, first = true, flip = false, logged = false;
    uint8_t brightness = 0;
    int fails = 0;

    // 電源を入れた直後は、OLED のリセットと電源の立ち上がりが終わるまで応えないことがある
    // （再起動のときは OLED に電源が入ったままなので、すぐ応える）
    vTaskDelay(pdMS_TO_TICKS(POWERUP_MS));
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(TICK_MS));
        int64_t now_ms = esp_timer_get_time() / 1000;
        status_t st = status_decide();

        // 見つかるまで（途中で書き込みに失敗したときも）探し直して、初期化からやり直す
        if (!s_ready) {
            if (now_ms < retry_at_ms) {
                continue;
            }
            flip = g_cfg.oled_flip;
            brightness = g_cfg.sled_brightness;
            if (oled_attach(flip, brightness, !logged) != ESP_OK) {
                logged = true;  // 外れたままのときにログが溢れないよう、1 回だけ出す
                retry_at_ms = now_ms + RETRY_MS;
                continue;
            }
            if (!start_ms) {
                start_ms = now_ms;  // 起動時の表示は、最初に見つかったときに出す
            }
            s_ready = true;
            logged = false;
            power = false;  // oled_init は表示を消した状態で終わる
            first = true;
        }
        if (now_ms - last_link_ms >= 1000) {
            net_get_link(ip, &rssi);
            last_link_ms = now_ms;
        }

        // 正常なときは、しばらくしたら画面を消す（操作・ボタン・常に表示の設定で出す）
        if (st == ST_OK) {
            if (ok_since_ms < 0) {
                ok_since_ms = now_ms;
            }
        } else {
            ok_since_ms = -1;
        }
        int64_t pulse_ms = status_pulse_at_us() / 1000, wake_ms = status_wake_at_us() / 1000;
        uint32_t held = status_button_held_ms();
        bool identify = now_ms * 1000 < status_identify_until_us();
        bool show = st != ST_OK || g_cfg.sled_always_on || now_ms - ok_since_ms < OK_HOLD_MS ||
                    (pulse_ms && now_ms - pulse_ms < PULSE_SHOW_MS) || (wake_ms && now_ms - wake_ms < WAKE_MS) ||
                    identify || held >= BUTTON_SHOW_MS || now_ms - start_ms < BOOT_MS;

        s_shift = (int)((now_ms / SHIFT_MS) % 3);
        memset(s_fb, 0, sizeof(s_fb));
        if (identify) {
            s_shift = 0;
            draw_identify(now_ms);
        } else if (held >= BUTTON_SHOW_MS) {
            s_shift = 0;
            draw_button(held);
        } else if (now_ms - start_ms < BOOT_MS) {
            draw_boot();
        } else {
            draw_header(st, rssi, ts_ip_shown(st, now_ms), now_ms);
            draw_status(st, ip, now_ms);
        }

        // 設定ページや Home Assistant で変えた明るさ・向きは、すぐに反映する
        esp_err_t err = ESP_OK;
        if (g_cfg.sled_brightness != brightness) {
            brightness = g_cfg.sled_brightness;
            err = set_brightness(brightness);
        }
        if (err == ESP_OK && g_cfg.oled_flip != flip) {
            flip = g_cfg.oled_flip;
            err = set_flip(flip);
        }
        if (err == ESP_OK && show && (first || memcmp(sent, s_fb, sizeof(s_fb)) != 0)) {
            err = send_frame();
            if (err == ESP_OK) {
                memcpy(sent, s_fb, sizeof(s_fb));
                first = false;
            }
        }
        if (err == ESP_OK && show != power) {
            err = set_power(show);
            if (err == ESP_OK) {
                power = show;
            }
        }
        if (err != ESP_OK) {
            // 配線が外れた・OLED の電源が切れたときは、設定が消えているので初期化からやり直す
            if (fails++ == 0) {
                ESP_LOGE(TAG, "OLED write failed: %s", esp_err_to_name(err));
            }
            s_ready = false;
            retry_at_ms = now_ms + RETRY_MS;
        } else {
            fails = 0;
        }
    }
}

bool status_oled_enabled(void)
{
    return s_ready;  // 探している間・外れている間は false（設定ページには「使わない」と出る）
}

void status_oled_start(void)
{
    if (g_cfg.oled_sda == STATUS_LED_DISABLED || g_cfg.oled_scl == STATUS_LED_DISABLED) {
        ESP_LOGI(TAG, "OLED disabled");
        return;
    }
    i2c_master_bus_config_t bus = {
        .i2c_port = -1,  // 空いているものを使う
        .sda_io_num = g_cfg.oled_sda,
        .scl_io_num = g_cfg.oled_scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,  // モジュールにプルアップ抵抗がないときの保険
    };
    if (i2c_new_master_bus(&bus, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed (SDA GPIO%u, SCL GPIO%u)", g_cfg.oled_sda, g_cfg.oled_scl);
        return;
    }
    // OLED を探すのは、電源の立ち上がりを待ってからタスクの中で行う（見つかるまで探し直す）
    xTaskCreate(oled_task, "status_oled", 4096, NULL, 3, NULL);
}
