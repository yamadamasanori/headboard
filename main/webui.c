#include "webui.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "app_config.h"
#include "commands.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logbuf.h"
#include "mdns.h"
#include "ota.h"
#include "usage.h"

static const char *TAG = "webui";

#define MAX_CLIENTS      4
#define WS_MAX_IN        4096    // 設定の保存（CA 証明書を含む）が入る大きさ
#define LOG_CHUNK        2048
#define LOG_HDR          9       // 0x00 + 位置 8 バイト
#define LOG_CHUNKS_MAX   4       // 1 回の送信で送る最大チャンク数（他の処理を待たせないため）
#define LOGIN_TRIES      5
#define LOGIN_LOCK_US    (60LL * 1000 * 1000)

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char license_start[] asm("_binary_LICENSE_start");
extern const char license_end[] asm("_binary_LICENSE_end");
extern const char notices_start[] asm("_binary_THIRD_PARTY_NOTICES_md_start");
extern const char notices_end[] asm("_binary_THIRD_PARTY_NOTICES_md_end");

typedef struct {
    int fd;          // -1 で空き
    bool authed;
    uint64_t log_pos;
} client_t;

// s_clients と s_fail_* は httpd のタスクの中からだけ触る（ハンドラと httpd_queue_work）
static httpd_handle_t s_server;
static client_t s_clients[MAX_CLIENTS];
static int s_fail_count;
static int64_t s_lock_until_us;
static char s_token[33];             // OTA の POST に付けてもらう合言葉（ログインで渡す）
static char s_boot_id[9];            // 起動ごとに変わる ID（再接続時にログの続きから送るため）
static volatile bool s_flush_queued;

// ---------------------------------------------------------------- クライアント管理

static client_t *find_client(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].fd == fd) {
            return &s_clients[i];
        }
    }
    return NULL;
}

static client_t *add_client(int fd)
{
    client_t *c = find_client(fd);
    if (!c) {
        c = find_client(-1);
    }
    if (c) {
        *c = (client_t){.fd = fd, .authed = false, .log_pos = 0};
    }
    return c;
}

static void remove_client(int fd)
{
    client_t *c = find_client(fd);
    if (c) {
        c->fd = -1;
        c->authed = false;
    }
}

static void on_close(httpd_handle_t hd, int fd)
{
    remove_client(fd);
    close(fd);
}

// ---------------------------------------------------------------- 送信

// 文字の途中で区切っても壊れないよう、すべてバイナリのフレームで送る（ページ側でまとめて UTF-8 にする）。
// 応答は "@@" で始まる 1 行。ログは先頭に 0x00 と、そのチャンクの終わりの位置（8 バイト、リトルエンディアン）を付けたもの
static esp_err_t ws_send_async(int fd, const void *data, size_t len)
{
    httpd_ws_frame_t f = {.type = HTTPD_WS_TYPE_BINARY, .payload = (uint8_t *)data, .len = len, .final = true};
    return httpd_ws_send_frame_async(s_server, fd, &f);
}

typedef struct {
    int fd;
} send_ctx_t;

static void send_line(const char *json, void *ctx)
{
    const send_ctx_t *sc = ctx;
    size_t n = strlen(json);
    char *buf = malloc(n + 4);
    if (!buf) {
        return;
    }
    memcpy(buf, "@@", 2);
    memcpy(buf + 2, json, n);
    buf[n + 2] = '\n';
    ws_send_async(sc->fd, buf, n + 3);
    free(buf);
}

static void send_obj(int fd, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    if (s) {
        send_ctx_t sc = {.fd = fd};
        send_line(s, &sc);
        free(s);
    }
    cJSON_Delete(o);
}

static void send_error(int fd, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "error");
    cJSON_AddStringToObject(o, "message", msg);
    send_obj(fd, o);
}

// ログイン済みのクライアントに、まだ送っていないログを送る（httpd のタスクで動く）
static void flush_logs(void *arg)
{
    s_flush_queued = false;
    static char buf[LOG_HDR + LOG_CHUNK];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &s_clients[i];
        if (c->fd < 0 || !c->authed) {
            continue;
        }
        if (httpd_ws_get_fd_info(s_server, c->fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            remove_client(c->fd);
            continue;
        }
        for (int k = 0; k < LOG_CHUNKS_MAX; k++) {
            size_t n = logbuf_read(&c->log_pos, buf + LOG_HDR, LOG_CHUNK);
            if (n == 0) {
                break;
            }
            buf[0] = 0;  // ログの印
            for (int b = 0; b < 8; b++) {
                buf[1 + b] = (char)(c->log_pos >> (8 * b));
            }
            if (ws_send_async(c->fd, buf, LOG_HDR + n) != ESP_OK) {
                remove_client(c->fd);
                break;
            }
        }
    }
    // 送り切れなかった分は次の周期で送る
}

static void log_pump_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!s_server || s_flush_queued) {
            continue;
        }
        bool pending = false;
        uint64_t end = logbuf_end();
        // s_clients は httpd のタスクのものだが、読むだけなので多少古くてもよい
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (s_clients[i].fd >= 0 && s_clients[i].authed && s_clients[i].log_pos < end) {
                pending = true;
            }
        }
        if (pending) {
            s_flush_queued = true;
            if (httpd_queue_work(s_server, flush_logs, NULL) != ESP_OK) {
                s_flush_queued = false;
            }
        }
    }
}

// ---------------------------------------------------------------- ページとファイル

static esp_err_t send_embedded(httpd_req_t *req, const char *type, const char *start, const char *end)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, start, end - start);
}

static esp_err_t h_index(httpd_req_t *req)
{
    return send_embedded(req, "text/html; charset=utf-8", index_html_start, index_html_end);
}

static esp_err_t h_license(httpd_req_t *req)
{
    return send_embedded(req, "text/plain; charset=utf-8", license_start, license_end);
}

static esp_err_t h_notices(httpd_req_t *req)
{
    return send_embedded(req, "text/plain; charset=utf-8", notices_start, notices_end);
}

// ページが「本体から配信されている」ことを知るための情報（秘密は含めない）
static esp_err_t h_info(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "firmware", "headboard-led");
    cJSON_AddStringToObject(o, "version", FW_VERSION);
    cJSON_AddStringToObject(o, "device_id", config_device_id());
    cJSON_AddStringToObject(o, "hostname", config_hostname());
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, s);
    free(s);
    return err;
}

// ---------------------------------------------------------------- WebSocket（設定・ログ）

static bool pop_matches(const char *given)
{
    const char *pop = config_pop();
    size_t n = strlen(pop);
    if (strlen(given) != n) {
        return false;
    }
    // 小文字で入力しても通す。比較は文字数分すべて行う
    unsigned diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned)(toupper((unsigned char)given[i]) ^ pop[i]);
    }
    return diff == 0;
}

static void handle_login(client_t *c, const cJSON *req)
{
    int64_t now = esp_timer_get_time();
    if (now < s_lock_until_us) {
        send_error(c->fd, "コードの入力を続けて間違えたため、1 分間ログインできません");
        return;
    }
    const cJSON *pop = cJSON_GetObjectItemCaseSensitive(req, "pop");
    if (!cJSON_IsString(pop) || !pop_matches(pop->valuestring)) {
        if (++s_fail_count >= LOGIN_TRIES) {
            s_fail_count = 0;
            s_lock_until_us = now + LOGIN_LOCK_US;
        }
        ESP_LOGW(TAG, "login failed");
        send_error(c->fd, "コードが違います（本体のラベルに書かれた 8 文字）");
        return;
    }
    s_fail_count = 0;
    c->authed = true;
    // 同じ起動のうちの再接続なら、前回受け取った続きから送る。そうでなければ残っている最も古いログから
    const cJSON *boot = cJSON_GetObjectItemCaseSensitive(req, "boot_id");
    const cJSON *since = cJSON_GetObjectItemCaseSensitive(req, "since");
    c->log_pos = 0;
    if (cJSON_IsString(boot) && strcmp(boot->valuestring, s_boot_id) == 0 && cJSON_IsNumber(since) && since->valuedouble > 0) {
        c->log_pos = (uint64_t)since->valuedouble;
    }
    ESP_LOGI(TAG, "login ok");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "login");
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "token", s_token);
    cJSON_AddStringToObject(o, "boot_id", s_boot_id);
    send_obj(c->fd, o);
}

static void handle_ws_line(client_t *c, const char *line)
{
    if (c->authed) {
        send_ctx_t sc = {.fd = c->fd};
        cmd_handle(line, CMD_SRC_WEB, send_line, &sc);
        return;
    }
    cJSON *req = cJSON_Parse(line);
    const cJSON *cmd = req ? cJSON_GetObjectItemCaseSensitive(req, "cmd") : NULL;
    if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "login") == 0) {
        handle_login(c, req);
    } else if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "hello") == 0) {
        send_obj(c->fd, cmd_hello(CMD_SRC_WEB));
    } else {
        send_error(c->fd, "ログインしてください");
    }
    cJSON_Delete(req);
}

static esp_err_t h_ws(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        if (!add_client(fd)) {
            ESP_LOGW(TAG, "too many clients");
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    client_t *c = find_client(fd);
    if (!c) {
        c = add_client(fd);
        if (!c) {
            return ESP_FAIL;
        }
    }

    httpd_ws_frame_t f = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (f.type != HTTPD_WS_TYPE_TEXT && f.type != HTTPD_WS_TYPE_BINARY) {
        return ESP_OK;
    }
    if (f.len == 0 || f.len > WS_MAX_IN) {
        send_error(fd, "1 行が長すぎます");
        return ESP_OK;
    }
    char *buf = malloc(f.len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    f.payload = (uint8_t *)buf;
    err = httpd_ws_recv_frame(req, &f, f.len);
    if (err == ESP_OK) {
        buf[f.len] = 0;
        // 1 フレームに複数行が入っていてもよい
        char *save = NULL;
        for (char *line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
            if (line[0]) {
                handle_ws_line(c, line);
            }
        }
    }
    free(buf);
    return err;
}

// ---------------------------------------------------------------- OTA（ファームウェア更新）

static void restart_cb(void *arg)
{
    usage_flush();
    esp_restart();
}

static esp_err_t ota_fail(httpd_req_t *req, const char *status, const char *msg)
{
    ESP_LOGW(TAG, "OTA rejected: %s", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t h_ota(httpd_req_t *req)
{
    char token[40] = "";
    if (httpd_req_get_hdr_value_str(req, "X-Token", token, sizeof(token)) != ESP_OK || strcmp(token, s_token) != 0) {
        return ota_fail(req, "403 Forbidden", "ログインしてください");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return ota_fail(req, "500 Internal Server Error", "更新用の領域がありません（パーティションを USB で書き直してください）");
    }
    if (req->content_len < OTA_HEADER_LEN || req->content_len > part->size) {
        return ota_fail(req, "400 Bad Request", "ファイルの大きさが正しくありません");
    }

    char *buf = malloc(4096);
    if (!buf) {
        return ota_fail(req, "500 Internal Server Error", "メモリが足りません");
    }
    esp_ota_handle_t ota = 0;
    size_t got = 0, have = 0;  // have: 先頭の確認が済むまでバッファに貯めた量
    int last_pct = -1;
    const char *fail = NULL;
    char why[96];
    ESP_LOGI(TAG, "OTA start: %u bytes to %s", (unsigned)req->content_len, part->label);
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + have, 4096 - have);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            fail = "受信が途中で切れました";
            break;
        }
        got += r;
        size_t len = have + r;
        if (!ota) {
            if (len < OTA_HEADER_LEN && got < req->content_len) {
                have = len;
                continue;
            }
            if (!ota_check_header((uint8_t *)buf, len, why, sizeof(why))) {
                fail = why;
                break;
            }
            if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
                fail = "書き込みを始められませんでした";
                break;
            }
        }
        have = 0;
        if (esp_ota_write(ota, buf, len) != ESP_OK) {
            fail = "書き込みに失敗しました";
            break;
        }
        int pct = (int)(got * 10 / req->content_len) * 10;
        if (pct != last_pct) {
            last_pct = pct;
            ESP_LOGI(TAG, "OTA %d%%", pct);
        }
    }
    free(buf);
    if (!fail && ota) {
        esp_err_t err = esp_ota_end(ota);
        ota = 0;
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            fail = "ファームウェアが壊れています（検証に失敗しました）";
        } else if (err != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
            fail = "書き込みの仕上げに失敗しました";
        }
    }
    if (ota) {
        esp_ota_abort(ota);
    }
    if (fail) {
        return ota_fail(req, "400 Bad Request", fail);
    }
    ESP_LOGI(TAG, "OTA done, restarting");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "更新しました。再起動します");
    // 応答を返し終えてから再起動する
    const esp_timer_create_args_t ta = {.callback = restart_cb, .name = "ota_restart"};
    esp_timer_handle_t t;
    if (esp_timer_create(&ta, &t) == ESP_OK) {
        esp_timer_start_once(t, 1500 * 1000);
    }
    return ESP_OK;
}

// ---------------------------------------------------------------- 起動

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed");
        return;
    }
    mdns_hostname_set(config_hostname());
    mdns_instance_name_set(g_cfg.device_name);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

void webui_start(void)
{
    if (s_server) {
        return;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        s_clients[i].fd = -1;
    }
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) {
        sprintf(s_token + i * 2, "%02x", rnd[i]);
    }
    snprintf(s_boot_id, sizeof(s_boot_id), "%08" PRIx32, esp_random());

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_open_sockets = 5;
    cfg.lru_purge_enable = true;
    cfg.close_fn = on_close;
    cfg.max_uri_handlers = 8;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_server = NULL;
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/index.html", .method = HTTP_GET, .handler = h_index},
        {.uri = "/LICENSE.txt", .method = HTTP_GET, .handler = h_license},
        {.uri = "/THIRD_PARTY_NOTICES.txt", .method = HTTP_GET, .handler = h_notices},
        {.uri = "/api/info", .method = HTTP_GET, .handler = h_info},
        {.uri = "/ws", .method = HTTP_GET, .handler = h_ws, .is_websocket = true},
        {.uri = "/ota", .method = HTTP_POST, .handler = h_ota},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    start_mdns();
    xTaskCreate(log_pump_task, "log_pump", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "setup page: http://%s.local/", config_hostname());
}
