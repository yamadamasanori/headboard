#include "leds.h"

#include <math.h>
#include <string.h>
#include "app_config.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usage.h"

static const char *TAG = "leds";

#define LEDC_MODE        LEDC_LOW_SPEED_MODE
#define LEDC_TIMER       LEDC_TIMER_0
#define LEDC_SRC_HZ      80000000u
#define FADE_STEP_MS     10
#define DEFAULT_FADE_MS  500
#define MAX_FADE_MS      60000
#define GAMMA            2.2f

typedef struct {
    bool on;
    uint8_t brightness;   // 最後に設定された明るさ（消灯中も保持し、再点灯時に使う）
    float level;          // 現在の出力レベル 0〜255（フェード中は途中の値）
    float from;
    float to;
    int64_t t_start_us;
    uint32_t dur_ms;
} light_t;

static const char *s_keys[LIGHT_COUNT] = {"reading", "backlight"};
static const char *s_names[LIGHT_COUNT] = {"読書灯", "バックライト"};

static light_t s_lights[LIGHT_COUNT];
static SemaphoreHandle_t s_lock;
static uint32_t s_max_duty;
static leds_changed_cb_t s_cb;

const char *leds_key(light_id_t id) { return s_keys[id]; }
const char *leds_display_name(light_id_t id) { return s_names[id]; }

int leds_find(const char *key)
{
    for (int i = 0; i < LIGHT_COUNT; i++) {
        if (strcmp(key, s_keys[i]) == 0) {
            return i;
        }
    }
    return -1;
}

void leds_set_changed_cb(leds_changed_cb_t cb) { s_cb = cb; }

// 人の目の感じ方に合わせて、明るさ(0〜255)をデューティ比に変換する
static uint32_t level_to_duty(float level)
{
    if (level <= 0.0f) {
        return 0;
    }
    uint32_t duty = (uint32_t)lroundf(powf(level / 255.0f, GAMMA) * s_max_duty);
    return duty == 0 ? 1 : duty;
}

static void write_duty(light_id_t id, float level)
{
    ledc_set_duty(LEDC_MODE, (ledc_channel_t)id, level_to_duty(level));
    ledc_update_duty(LEDC_MODE, (ledc_channel_t)id);
}

static void fade_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(FADE_STEP_MS));
        int64_t now = esp_timer_get_time();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < LIGHT_COUNT; i++) {
            light_t *l = &s_lights[i];
            if (l->level == l->to) {
                continue;
            }
            float t = l->dur_ms ? (float)(now - l->t_start_us) / (l->dur_ms * 1000.0f) : 1.0f;
            l->level = t >= 1.0f ? l->to : l->from + (l->to - l->from) * t;
            write_duty(i, l->level);
        }
        xSemaphoreGive(s_lock);
    }
}

static int duty_resolution_bits(uint32_t freq)
{
    int bits = 13;
    while (bits > 8 && ((uint64_t)freq << bits) > LEDC_SRC_HZ) {
        bits--;
    }
    return bits;
}

void leds_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < LIGHT_COUNT; i++) {
        s_lights[i] = (light_t){.on = false, .brightness = 255};
    }

    int bits = duty_resolution_bits(g_cfg.pwm_freq);
    s_max_duty = (1u << bits) - 1;
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = (ledc_timer_bit_t)bits,
        .timer_num = LEDC_TIMER,
        .freq_hz = g_cfg.pwm_freq,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    const uint8_t pins[LIGHT_COUNT] = {g_cfg.pin_reading, g_cfg.pin_backlight};
    for (int i = 0; i < LIGHT_COUNT; i++) {
        ledc_channel_config_t ch = {
            .gpio_num = pins[i],
            .speed_mode = LEDC_MODE,
            .channel = (ledc_channel_t)i,
            .timer_sel = LEDC_TIMER,
            .duty = 0,
            .hpoint = 0,
            .flags.output_invert = g_cfg.pwm_invert ? 1 : 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch));
    }
    ESP_LOGI(TAG, "PWM %u Hz, %d bit, reading=GPIO%u, backlight=GPIO%u%s",
             g_cfg.pwm_freq, bits, g_cfg.pin_reading, g_cfg.pin_backlight,
             g_cfg.pwm_invert ? " (inverted)" : "");

    xTaskCreate(fade_task, "led_fade", 3072, NULL, 5, NULL);
}

void leds_set(light_id_t id, bool on, int brightness, uint32_t transition_ms)
{
    if (brightness > 255) {
        brightness = 255;
    }
    if (transition_ms > MAX_FADE_MS) {
        transition_ms = MAX_FADE_MS;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    light_t *l = &s_lights[id];
    bool was_on = l->on;
    if (on && brightness == 0) {
        on = false;
    } else if (brightness > 0) {
        l->brightness = brightness;
    }
    l->on = on;
    l->from = l->level;
    l->to = on ? l->brightness : 0;
    l->t_start_us = esp_timer_get_time();
    l->dur_ms = transition_ms;
    xSemaphoreGive(s_lock);
    if (on && !was_on) {
        usage_light_turned_on(id);
    }
    if (s_cb) {
        s_cb(id);
    }
}

bool leds_apply_command(light_id_t id, const char *payload, int len)
{
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) {
        if (len == 2 && strncmp(payload, "ON", 2) == 0) {
            leds_set(id, true, -1, DEFAULT_FADE_MS);
            return true;
        }
        if (len == 3 && strncmp(payload, "OFF", 3) == 0) {
            leds_set(id, false, -1, DEFAULT_FADE_MS);
            return true;
        }
        ESP_LOGW(TAG, "invalid command: %.*s", len, payload);
        return false;
    }

    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *bri = cJSON_GetObjectItemCaseSensitive(root, "brightness");
    const cJSON *tr = cJSON_GetObjectItemCaseSensitive(root, "transition");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool on = s_lights[id].on;
    xSemaphoreGive(s_lock);
    int brightness = -1;
    uint32_t fade_ms = DEFAULT_FADE_MS;

    if (cJSON_IsString(state)) {
        on = strcmp(state->valuestring, "ON") == 0;
    }
    if (cJSON_IsNumber(bri)) {
        brightness = bri->valueint < 0 ? 0 : bri->valueint;
        if (!cJSON_IsString(state)) {
            on = brightness > 0;
        }
    }
    if (cJSON_IsNumber(tr) && tr->valuedouble >= 0) {
        fade_ms = (uint32_t)(tr->valuedouble * 1000.0);
    }
    cJSON_Delete(root);

    leds_set(id, on, brightness, fade_ms);
    return true;
}

float leds_output_ratio(light_id_t id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    float level = s_lights[id].level;
    xSemaphoreGive(s_lock);
    return s_max_duty ? (float)level_to_duty(level) / s_max_duty : 0.0f;
}

void leds_state_to_json(light_id_t id, cJSON *o)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    light_t l = s_lights[id];
    xSemaphoreGive(s_lock);
    cJSON_AddStringToObject(o, "state", l.on ? "ON" : "OFF");
    cJSON_AddNumberToObject(o, "brightness", l.brightness);
    cJSON_AddStringToObject(o, "color_mode", "brightness");
}

char *leds_state_json(light_id_t id)
{
    cJSON *o = cJSON_CreateObject();
    leds_state_to_json(id, o);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}
