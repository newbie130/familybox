#include "fb_store.h"

#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fb_config.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"

static const char *TAG = "fb_store";
static const char *NVS_NS = "familybox";
static const char *NVS_KEY_WATERMARK = "watermark";

static fb_message_t s_current;
static int16_t *s_reply_slots[FB_REPLY_SLOTS];
static bool s_slot_used[FB_REPLY_SLOTS];
static SemaphoreHandle_t s_slot_mutex;
static int32_t s_watermark;

esp_err_t fb_store_init(void)
{
    /* All three buffers are allocated once, up front. Allocating a 1.9 MB
     * buffer lazily at the moment a message arrives is a good way to
     * discover PSRAM fragmentation at the worst possible time. */
    s_current.photo = heap_caps_malloc(FB_PHOTO_BYTES, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_current.photo, ESP_ERR_NO_MEM, TAG, "photo buffer alloc failed");

    s_current.audio = heap_caps_malloc(FB_PLAY_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_current.audio, ESP_ERR_NO_MEM, TAG, "play buffer alloc failed");

    s_slot_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_slot_mutex, ESP_ERR_NO_MEM, TAG, "slot mutex alloc failed");
    for (int i = 0; i < FB_REPLY_SLOTS; i++) {
        s_reply_slots[i] = heap_caps_malloc(FB_RECORD_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        ESP_RETURN_ON_FALSE(s_reply_slots[i], ESP_ERR_NO_MEM, TAG, "reply slot %d alloc failed", i);
    }

    memset(s_current.photo, 0, FB_PHOTO_BYTES);
    s_current.id = 0;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_i32(nvs, NVS_KEY_WATERMARK, &s_watermark) != ESP_OK) {
            s_watermark = 0;
        }
        nvs_close(nvs);
    }

    ESP_LOGI(TAG, "buffers ready (photo %u KB, play %u KB, record %u KB), watermark=%ld",
             (unsigned)(FB_PHOTO_BYTES / 1024),
             (unsigned)(FB_PLAY_SAMPLES * sizeof(int16_t) / 1024),
             (unsigned)(FB_REPLY_SLOTS * FB_RECORD_SAMPLES * sizeof(int16_t) / 1024),
             (long)s_watermark);
    return ESP_OK;
}

fb_message_t *fb_store_current(void)
{
    return &s_current;
}

void fb_store_begin(int32_t id)
{
    s_current.id = id;
    s_current.has_photo = false;
    s_current.has_audio = false;
    s_current.photo_len = 0;
    s_current.audio_samples = 0;
}

int fb_store_reply_acquire(void)
{
    int slot = -1;
    xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
    for (int i = 0; i < FB_REPLY_SLOTS; i++) {
        if (!s_slot_used[i]) {
            s_slot_used[i] = true;
            slot = i;
            break;
        }
    }
    xSemaphoreGive(s_slot_mutex);
    return slot;
}

int16_t *fb_store_reply_buf(int slot)
{
    return (slot >= 0 && slot < FB_REPLY_SLOTS) ? s_reply_slots[slot] : NULL;
}

void fb_store_reply_release(int slot)
{
    if (slot < 0 || slot >= FB_REPLY_SLOTS) {
        return;
    }
    xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
    s_slot_used[slot] = false;
    xSemaphoreGive(s_slot_mutex);
}

int fb_store_reply_pending(void)
{
    int n = 0;
    xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
    for (int i = 0; i < FB_REPLY_SLOTS; i++) {
        if (s_slot_used[i]) n++;
    }
    xSemaphoreGive(s_slot_mutex);
    return n;
}

size_t fb_store_record_capacity(void)
{
    return FB_RECORD_SAMPLES;
}

int32_t fb_store_watermark(void)
{
    return s_watermark;
}

esp_err_t fb_store_set_watermark(int32_t id)
{
    if (id <= s_watermark) {
        return ESP_OK;
    }
    s_watermark = id;

    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &nvs), TAG, "nvs open failed");
    esp_err_t err = nvs_set_i32(nvs, NVS_KEY_WATERMARK, id);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}
