#pragma once

// ボードごとの違い（CMakeLists.txt の HB_BOARD → sdkconfig.defaults.<ボード> で選ぶ）
#include "sdkconfig.h"

#if CONFIG_HEADBOARD_BOARD_XIAO
// Seeed Studio XIAO ESP32S3。状態表示は SSD1306 の OLED（I2C）
#define BOARD_ID            "xiao"
#define BOARD_NAME          "XIAO ESP32S3"
#define BOARD_FW_PREFIX     "headboard-led-xiao"    // 配布するファイル名の先頭（tools/package_web.py と合わせる）
#define BOARD_HAS_OLED      1
#define BOARD_MODEL         "ESP32 Headboard LED (XIAO ESP32S3)"
#define BOARD_STATUS_NAME   "画面"                  // Home Assistant での状態表示の呼び名
#define BOARD_STATUS_ON     "表示"
// 専用基板の配線に合わせる。D7（GPIO44）は UART0 の RX だが、XIAO では COM 端子からの
// 入力を使わない（console.c）ので、バックライトに使う
#define BOARD_PIN_READING   7   // D8（LED1）
#define BOARD_PIN_BACKLIGHT 44  // D7（LED2・LED3）
#define BOARD_OLED_SDA      8   // D9
#define BOARD_OLED_SCL      9   // D10
#define BOARD_UART_RX_FREE  1   // GPIO44（UART0 RX）を出力に使ってよい
#else
// ESP32-S3 N16R8（DevKitC-1 互換）。状態表示は基板上のフルカラー LED（WS2812）
#define BOARD_ID            "n16r8"
#define BOARD_NAME          "ESP32-S3 N16R8"
#define BOARD_FW_PREFIX     "headboard-led"
#define BOARD_HAS_OLED      0
#define BOARD_MODEL         "ESP32 Headboard LED"
#define BOARD_STATUS_NAME   "状態表示 LED"
#define BOARD_STATUS_ON     "点灯"
#define BOARD_PIN_READING   4
#define BOARD_PIN_BACKLIGHT 5
#define BOARD_UART_RX_FREE  0   // GPIO44 は COM 端子の RX（設定用の入力）
#define BOARD_SLED_PIN      48
#endif
