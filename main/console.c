#include "console.h"

#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "driver/usb_serial_jtag.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "leds.h"
#include "net.h"
#include "status_led.h"
#include "tailscale.h"

#define CONSOLE_UART CONFIG_ESP_CONSOLE_UART_NUM
#define CMD_LINE_MAX     1024

static const char *TAG = "console";

// COM 端子（UART）と USB 端子（ESP32-S3 内蔵 USB）の両方から同時にコマンドが来ても混ざらないように
static SemaphoreHandle_t s_lock;

// 応答は両方の端子に送る。USB 端子は PC が読んでいなければ捨てる（待たない）
static void send(cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    printf("@@%s\n", s);
    fflush(stdout);
    usb_serial_jtag_write_bytes("@@", 2, 0);
    usb_serial_jtag_write_bytes(s, strlen(s), 0);
    usb_serial_jtag_write_bytes("\n", 1, 0);
    free(s);
    cJSON_Delete(o);
}

static cJSON *reply(const char *type)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", type);
    return o;
}

static void send_error(const char *msg)
{
    cJSON *o = reply("error");
    cJSON_AddStringToObject(o, "message", msg);
    send(o);
}

static void send_hello(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cJSON *o = reply("hello");
    cJSON_AddStringToObject(o, "firmware", "headboard-led");
    cJSON_AddStringToObject(o, "version", FW_VERSION);
    cJSON_AddStringToObject(o, "device_id", config_device_id());
    cJSON_AddStringToObject(o, "chip", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(o, "chip_revision", chip.revision);
    send(o);
}

static void send_config(void)
{
    cJSON *o = reply("config");
    config_to_json(cJSON_AddObjectToObject(o, "config"));
    send(o);
}

static void send_status(void)
{
    cJSON *o = reply("status");
    net_status_to_json(o);
    cJSON *lights = cJSON_AddObjectToObject(o, "lights");
    for (int i = 0; i < LIGHT_COUNT; i++) {
        leds_state_to_json(i, cJSON_AddObjectToObject(lights, leds_key(i)));
    }
    cJSON_AddStringToObject(o, "status_led", status_led_indicator());
    cJSON_AddNumberToObject(o, "free_heap", esp_get_free_heap_size());
    send(o);
}

static void restart_later(void)
{
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void handle_line(const char *line)
{
    cJSON *req = cJSON_Parse(line);
    const cJSON *cmd = req ? cJSON_GetObjectItemCaseSensitive(req, "cmd") : NULL;
    if (!cJSON_IsString(cmd)) {
        if (line[0] == '{') {
            send_error("JSON を解釈できません");
        } else {
            // 手入力向けの簡易ヘルプ
            printf("commands: {\"cmd\":\"hello|get|status|set|light|reboot|factory_reset\"}\n");
        }
        cJSON_Delete(req);
        return;
    }
    const char *c = cmd->valuestring;

    if (strcmp(c, "hello") == 0) {
        send_hello();
    } else if (strcmp(c, "get") == 0) {
        send_config();
    } else if (strcmp(c, "status") == 0) {
        send_status();
    } else if (strcmp(c, "set") == 0) {
        const cJSON *cfg = cJSON_GetObjectItemCaseSensitive(req, "config");
        char err[96] = "";
        if (!cJSON_IsObject(cfg)) {
            send_error("config がありません");
        } else if (!config_from_json(cfg, err, sizeof(err))) {
            send_error(err);
        } else if (config_save() != ESP_OK) {
            send_error("設定の保存に失敗しました");
        } else {
            config_apply_log_levels();
            cJSON *o = reply("saved");
            config_to_json(cJSON_AddObjectToObject(o, "config"));
            send(o);
            // 保存した設定は再起動後に反映する
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "reboot"))) {
                restart_later();
            }
        }
    } else if (strcmp(c, "light") == 0) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
        int idx = cJSON_IsString(id) ? leds_find(id->valuestring) : -1;
        if (idx < 0) {
            send_error("id は reading または backlight を指定してください");
        } else {
            char *payload = cJSON_PrintUnformatted(req);
            leds_apply_command(idx, payload, strlen(payload));
            free(payload);
            send_status();
        }
    } else if (strcmp(c, "reboot") == 0) {
        send(reply("rebooting"));
        restart_later();
    } else if (strcmp(c, "factory_reset") == 0) {
        config_factory_reset();
        ts_factory_reset();
        send(reply("rebooting"));
        restart_later();
    } else {
        send_error("不明なコマンドです");
    }
    cJSON_Delete(req);
}

typedef struct {
    char line[CMD_LINE_MAX];
    size_t len;
    bool overflow;
} line_buf_t;

static void feed(line_buf_t *lb, const uint8_t *buf, int n)
{
    for (int i = 0; i < n; i++) {
        char ch = buf[i];
        if (ch == '\r' || ch == '\n') {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (lb->overflow) {
                send_error("1 行が長すぎます");
            } else if (lb->len > 0) {
                lb->line[lb->len] = 0;
                handle_line(lb->line);
            }
            xSemaphoreGive(s_lock);
            lb->len = 0;
            lb->overflow = false;
        } else if (lb->len < CMD_LINE_MAX - 1) {
            lb->line[lb->len++] = ch;
        } else {
            lb->overflow = true;
        }
    }
}

static void uart_task(void *arg)
{
    static line_buf_t lb;
    uint8_t buf[64];
    send_hello();
    for (;;) {
        int n = uart_read_bytes(CONSOLE_UART, buf, sizeof(buf), pdMS_TO_TICKS(100));
        feed(&lb, buf, n);
    }
}

static void usb_task(void *arg)
{
    static line_buf_t lb;
    uint8_t buf[64];
    for (;;) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(100));
        feed(&lb, buf, n);
    }
}

void console_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, 2048, 0, 0, NULL, 0));
    uart_vfs_dev_use_driver(CONSOLE_UART);
    xTaskCreate(uart_task, "console_uart", 6144, NULL, 4, NULL);
    usb_serial_jtag_driver_config_t usj = {.rx_buffer_size = 1024, .tx_buffer_size = 2048};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usj));
    xTaskCreate(usb_task, "console_usb", 6144, NULL, 4, NULL);
    ESP_LOGI(TAG, "setup console ready on UART%d and USB", CONSOLE_UART);
}
