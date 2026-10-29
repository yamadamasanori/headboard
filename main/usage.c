#include "usage.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "nvs.h"

static const char *TAG = "usage";
static const char *NS = "stats";

#define TICK_MS        1000
#define SAVE_EVERY_S   600       // フラッシュの書き換えを抑えるため、10 分ごとにまとめて保存する

typedef struct {
    uint32_t boot_count;
    uint32_t brownout_count;
    uint32_t abnormal_count;     // クラッシュ・ウォッチドッグによる再起動
    uint64_t on_time_s[LIGHT_COUNT];
    uint32_t on_count[LIGHT_COUNT];
    uint64_t energy_uwh[LIGHT_COUNT];  // マイクロワット時
} usage_t;

static usage_t s_st;
static float s_power_w[LIGHT_COUNT];  // いまの推定消費電力
static bool s_abnormal_reset;
static bool s_dirty;
static uint32_t s_since_save_s;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static const char *key_for(const char *name, light_id_t id, char *buf, size_t len)
{
    snprintf(buf, len, "%s_%d", name, (int)id);  // NVS のキーは 15 文字まで
    return buf;
}

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    char k[16];
    nvs_get_u32(h, "boots", &s_st.boot_count);
    nvs_get_u32(h, "brownouts", &s_st.brownout_count);
    nvs_get_u32(h, "abnormal", &s_st.abnormal_count);
    for (int i = 0; i < LIGHT_COUNT; i++) {
        nvs_get_u64(h, key_for("on_s", i, k, sizeof(k)), &s_st.on_time_s[i]);
        nvs_get_u32(h, key_for("on_n", i, k, sizeof(k)), &s_st.on_count[i]);
        nvs_get_u64(h, key_for("uwh", i, k, sizeof(k)), &s_st.energy_uwh[i]);
    }
    nvs_close(h);
}

static void save(void)
{
    usage_t st;
    taskENTER_CRITICAL(&s_mux);
    st = s_st;
    s_dirty = false;
    s_since_save_s = 0;
    taskEXIT_CRITICAL(&s_mux);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "cannot open NVS");
        return;
    }
    char k[16];
    nvs_set_u32(h, "boots", st.boot_count);
    nvs_set_u32(h, "brownouts", st.brownout_count);
    nvs_set_u32(h, "abnormal", st.abnormal_count);
    for (int i = 0; i < LIGHT_COUNT; i++) {
        nvs_set_u64(h, key_for("on_s", i, k, sizeof(k)), st.on_time_s[i]);
        nvs_set_u32(h, key_for("on_n", i, k, sizeof(k)), st.on_count[i]);
        nvs_set_u64(h, key_for("uwh", i, k, sizeof(k)), st.energy_uwh[i]);
    }
    nvs_commit(h);
    nvs_close(h);
}

// 1 秒ごとに、点灯時間と電力量を積算する
static void tick(TimerHandle_t t)
{
    const float rated_w[LIGHT_COUNT] = {g_cfg.watt_reading, g_cfg.watt_backlight};
    float ratio[LIGHT_COUNT];
    for (int i = 0; i < LIGHT_COUNT; i++) {
        ratio[i] = leds_output_ratio(i);
    }
    bool need_save;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < LIGHT_COUNT; i++) {
        // PWM の平均電力は、おおよそ定格 × デューティ比
        s_power_w[i] = rated_w[i] * ratio[i];
        if (ratio[i] > 0.0f) {
            s_st.on_time_s[i]++;
            s_st.energy_uwh[i] += (uint64_t)(s_power_w[i] * 1e6f / 3600.0f + 0.5f);
            s_dirty = true;
        }
    }
    s_since_save_s++;
    need_save = s_dirty && s_since_save_s >= SAVE_EVERY_S;
    taskEXIT_CRITICAL(&s_mux);
    if (need_save) {
        save();
    }
}

void usage_init(void)
{
    load();
    esp_reset_reason_t r = esp_reset_reason();
    s_st.boot_count++;
    if (r == ESP_RST_BROWNOUT) {
        s_st.brownout_count++;
    }
    if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT) {
        s_st.abnormal_count++;
    }
    s_abnormal_reset = r == ESP_RST_BROWNOUT || r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
                       r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
    save();
    ESP_LOGI(TAG, "boot #%lu (brownouts %lu, crashes %lu)", (unsigned long)s_st.boot_count,
             (unsigned long)s_st.brownout_count, (unsigned long)s_st.abnormal_count);

    TimerHandle_t t = xTimerCreate("usage", pdMS_TO_TICKS(TICK_MS), pdTRUE, NULL, tick);
    xTimerStart(t, 0);
}

void usage_light_turned_on(light_id_t id)
{
    taskENTER_CRITICAL(&s_mux);
    s_st.on_count[id]++;
    s_dirty = true;
    taskEXIT_CRITICAL(&s_mux);
}

void usage_flush(void)
{
    if (s_dirty) {
        save();
    }
}

void usage_reset(void)
{
    taskENTER_CRITICAL(&s_mux);
    memset(&s_st, 0, sizeof(s_st));
    s_dirty = false;
    taskEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

void usage_to_json(cJSON *o)
{
    usage_t st;
    float power[LIGHT_COUNT];
    taskENTER_CRITICAL(&s_mux);
    st = s_st;
    memcpy(power, s_power_w, sizeof(power));
    taskEXIT_CRITICAL(&s_mux);

    cJSON_AddNumberToObject(o, "boot_count", st.boot_count);
    cJSON_AddNumberToObject(o, "brownout_count", st.brownout_count);
    cJSON_AddNumberToObject(o, "abnormal_reset_count", st.abnormal_count);
    cJSON_AddStringToObject(o, "abnormal_reset", s_abnormal_reset ? "ON" : "OFF");
    cJSON *lights = cJSON_AddObjectToObject(o, "lights");
    for (int i = 0; i < LIGHT_COUNT; i++) {
        cJSON *l = cJSON_AddObjectToObject(lights, leds_key(i));
        cJSON_AddNumberToObject(l, "on_time_h", (double)st.on_time_s[i] / 3600.0);
        cJSON_AddNumberToObject(l, "on_count", st.on_count[i]);
        cJSON_AddNumberToObject(l, "power_w", power[i]);
        cJSON_AddNumberToObject(l, "energy_wh", (double)st.energy_uwh[i] / 1e6);
    }
}
