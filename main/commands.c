#include "commands.h"

#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "leds.h"
#include "logbuf.h"
#include "net.h"
#include "usage.h"
#include "status_led.h"
#include "tailscale.h"

#ifdef CONFIG_HEADBOARD_USB_SETUP
#define USB_SETUP true
#else
#define USB_SETUP false
#endif

typedef struct {
    cmd_src_t src;
    cmd_send_fn_t fn;
    void *ctx;
} sink_t;

static void send(const sink_t *k, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    if (s) {
        k->fn(s, k->ctx);
        free(s);
    }
    cJSON_Delete(o);
}

static cJSON *reply(const char *type)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", type);
    return o;
}

static void send_error(const sink_t *k, const char *msg)
{
    cJSON *o = reply("error");
    cJSON_AddStringToObject(o, "message", msg);
    send(k, o);
}

cJSON *cmd_hello(cmd_src_t src)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cJSON *o = reply("hello");
    cJSON_AddStringToObject(o, "firmware", "headboard-led");
    cJSON_AddStringToObject(o, "version", FW_VERSION);
    cJSON_AddStringToObject(o, "device_id", config_device_id());
    cJSON_AddStringToObject(o, "chip", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(o, "chip_revision", chip.revision);
    cJSON_AddBoolToObject(o, "usb_setup", USB_SETUP);
    cJSON_AddStringToObject(o, "hostname", config_hostname());
    cJSON_AddStringToObject(o, "prov_name", config_prov_name());
    if (src == CMD_SRC_USB) {
        // ラベル印刷用。USB をつないでいる人（本体が手元にある人）にだけ見せる
        cJSON_AddStringToObject(o, "pop", config_pop());
    }
    return o;
}

static void send_config(const sink_t *k)
{
    cJSON *o = reply("config");
    config_to_json(cJSON_AddObjectToObject(o, "config"));
    send(k, o);
}

static void send_status(const sink_t *k)
{
    cJSON *o = reply("status");
    net_status_to_json(o);
    cJSON *lights = cJSON_AddObjectToObject(o, "lights");
    for (int i = 0; i < LIGHT_COUNT; i++) {
        leds_state_to_json(i, cJSON_AddObjectToObject(lights, leds_key(i)));
    }
    cJSON_AddStringToObject(o, "status_led", status_led_indicator());
    cJSON_AddNumberToObject(o, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(o, "reset_reason", logbuf_reset_reason());
    cJSON_AddStringToObject(o, "last_error", logbuf_last_error());
    cJSON_AddStringToObject(o, "prev_error", logbuf_prev_error());
    send(k, o);
}

static void restart_later(void)
{
    vTaskDelay(pdMS_TO_TICKS(300));
    usage_flush();
    esp_restart();
}

// 量産版の USB で受け付けるもの（設定を変えないもの）
static bool allowed(cmd_src_t src, const char *c)
{
    if (src != CMD_SRC_USB || USB_SETUP) {
        return true;
    }
    return strcmp(c, "hello") == 0 || strcmp(c, "status") == 0 || strcmp(c, "light") == 0 ||
           strcmp(c, "prev_log") == 0;
}

void cmd_handle(const char *line, cmd_src_t src, cmd_send_fn_t fn, void *ctx)
{
    const sink_t k = {.src = src, .fn = fn, .ctx = ctx};
    cJSON *req = cJSON_Parse(line);
    const cJSON *cmd = req ? cJSON_GetObjectItemCaseSensitive(req, "cmd") : NULL;
    if (!cJSON_IsString(cmd)) {
        if (line[0] == '{') {
            send_error(&k, "JSON を解釈できません");
        } else if (src == CMD_SRC_USB) {
            // 手入力向けの簡易ヘルプ
            printf("commands: {\"cmd\":\"hello|get|status|set|light|prev_log|reboot|reset_usage|factory_reset\"}\n");
        }
        cJSON_Delete(req);
        return;
    }
    const char *c = cmd->valuestring;

    if (!allowed(src, c)) {
        send_error(&k, "この本体は USB から設定を変更できません。Bluetooth と本体の設定ページを使ってください");
    } else if (strcmp(c, "hello") == 0) {
        send(&k, cmd_hello(src));
    } else if (strcmp(c, "get") == 0) {
        send_config(&k);
    } else if (strcmp(c, "status") == 0) {
        send_status(&k);
    } else if (strcmp(c, "prev_log") == 0) {
        cJSON *o = reply("prev_log");
        cJSON_AddStringToObject(o, "reset_reason", logbuf_reset_reason());
        cJSON_AddStringToObject(o, "text", logbuf_prev_tail());
        send(&k, o);
    } else if (strcmp(c, "set") == 0) {
        const cJSON *cfg = cJSON_GetObjectItemCaseSensitive(req, "config");
        char err[96] = "";
        if (!cJSON_IsObject(cfg)) {
            send_error(&k, "config がありません");
        } else if (!config_from_json(cfg, err, sizeof(err))) {
            send_error(&k, err);
        } else if (config_save() != ESP_OK) {
            send_error(&k, "設定の保存に失敗しました");
        } else {
            config_apply_log_levels();
            cJSON *o = reply("saved");
            config_to_json(cJSON_AddObjectToObject(o, "config"));
            send(&k, o);
            // 保存した設定は再起動後に反映する
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "reboot"))) {
                restart_later();
            }
        }
    } else if (strcmp(c, "light") == 0) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
        int idx = cJSON_IsString(id) ? leds_find(id->valuestring) : -1;
        if (idx < 0) {
            send_error(&k, "id は reading または backlight を指定してください");
        } else {
            char *payload = cJSON_PrintUnformatted(req);
            leds_apply_command(idx, payload, strlen(payload));
            free(payload);
            send_status(&k);
        }
    } else if (strcmp(c, "reboot") == 0) {
        send(&k, reply("rebooting"));
        restart_later();
    } else if (strcmp(c, "reset_usage") == 0) {
        usage_reset();
        send(&k, reply("usage_reset"));
    } else if (strcmp(c, "factory_reset") == 0) {
        config_factory_reset();
        ts_factory_reset();
        send(&k, reply("rebooting"));
        restart_later();
    } else {
        send_error(&k, "不明なコマンドです");
    }
    cJSON_Delete(req);
}
