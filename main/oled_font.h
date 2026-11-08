#pragma once

#include <stdint.h>

// ASCII（0x20〜0x7E）の等幅フォント。1 文字は h バイトで、1 バイトが 1 行（最上位ビットが左端）
typedef struct {
    uint8_t w, h;
    const uint8_t *data;
} oled_font_t;

#define OLED_FONT_FIRST 0x20
#define OLED_FONT_LAST  0x7E

extern const oled_font_t oled_font_small;  // 5×8（上段のホスト名・アイコンの文字。1 行 16 文字で 80 ドット）
extern const oled_font_t oled_font_large;  // 8×13 太字（本文。1 行 16 文字）
