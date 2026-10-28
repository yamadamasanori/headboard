#include "console.h"

#include <stdio.h>
#include <string.h>
#include "commands.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define CONSOLE_UART CONFIG_ESP_CONSOLE_UART_NUM
#define CMD_LINE_MAX     1024

static const char *TAG = "console";

// COM 端子（UART）と USB 端子（ESP32-S3 内蔵 USB）の両方から同時にコマンドが来ても混ざらないように
static SemaphoreHandle_t s_lock;

// 応答は両方の端子に送る。USB 端子は PC が読んでいなければ捨てる（待たない）
static void send_line(const char *json, void *ctx)
{
    printf("@@%s\n", json);
    fflush(stdout);
    usb_serial_jtag_write_bytes("@@", 2, 0);
    usb_serial_jtag_write_bytes(json, strlen(json), 0);
    usb_serial_jtag_write_bytes("\n", 1, 0);
}

static void send_error(const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "error");
    cJSON_AddStringToObject(o, "message", msg);
    char *s = cJSON_PrintUnformatted(o);
    send_line(s, NULL);
    free(s);
    cJSON_Delete(o);
}

static void send_hello(void)
{
    cJSON *o = cmd_hello(CMD_SRC_USB);
    char *s = cJSON_PrintUnformatted(o);
    send_line(s, NULL);
    free(s);
    cJSON_Delete(o);
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
                cmd_handle(lb->line, CMD_SRC_USB, send_line, NULL);
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
