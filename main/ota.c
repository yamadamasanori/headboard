#include "ota.h"

#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "ota";

void ota_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "updated firmware marked as valid");
    }
}

bool ota_check_header(const uint8_t *buf, size_t len, char *err, size_t err_len)
{
    const size_t desc_off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (len < desc_off + sizeof(esp_app_desc_t) || buf[0] != ESP_IMAGE_HEADER_MAGIC) {
        snprintf(err, err_len, "ESP32 のファームウェアではありません");
        return false;
    }
    const esp_app_desc_t *d = (const esp_app_desc_t *)(buf + desc_off);
    if (d->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        // 先頭がブートローダー（USB 書き込み用の headboard-led-boot.bin や、2.3.0 までの結合ファイル）だとここに来る
        snprintf(err, err_len, "更新用のファイルではありません。headboard-led-ota.bin を選んでください");
        return false;
    }
    const esp_app_desc_t *self = esp_app_get_description();
    if (strncmp(d->project_name, self->project_name, sizeof(d->project_name)) != 0) {
        snprintf(err, err_len, "別のプロジェクトのファームウェアです（%.32s）", d->project_name);
        return false;
    }
    const esp_image_header_t *h = (const esp_image_header_t *)buf;
    if (h->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        snprintf(err, err_len, "別のチップ用のファームウェアです");
        return false;
    }
    ESP_LOGI(TAG, "update image: %s %s (%s %s)", d->project_name, d->version, d->date, d->time);
    return true;
}
