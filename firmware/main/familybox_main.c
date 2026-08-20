/* Familybox: a photo-and-voice link between a travelling parent and a
 * five-year-old who cannot read yet.
 *
 * The device only ever does three things: poll the relay for a new message,
 * show it and play it, and record a reply when the big button is tapped.
 * Everything that requires decoding or judgement happens on the relay.
 */
#include <stdbool.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "fb_audio.h"
#include "fb_config.h"
#include "fb_net.h"
#include "fb_power.h"
#include "fb_store.h"
#include "fb_ui.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"

static const char *TAG = "familybox";

/* Record in short slices so the countdown ring animates and a stop request
 * is honoured promptly. */
#define RECORD_SLICE_SAMPLES (FB_SAMPLE_RATE / 4)

typedef struct {
    int slot;
    size_t samples;
} fb_pending_t;

static SemaphoreHandle_t s_tap;
static SemaphoreHandle_t s_play;
static QueueHandle_t s_upload_q;
static volatile bool s_recording;
static volatile bool s_busy;

static void on_record_tap(void)
{
    if (s_recording) {
        s_recording = false;   /* stop; the worker notices on its next slice */
        return;
    }
    if (!s_busy) {
        xSemaphoreGive(s_tap);
    }
}

static void on_play_tap(void)
{
    /* Deliberately repeatable: she can play Dad's note as many times as she
     * likes. Only blocked while something else is already using the speaker. */
    if (!s_busy) {
        xSemaphoreGive(s_play);
    }
}

/* ------------------------------------------------------------- inbox side */

static void deliver(const fb_inbox_entry_t *entry)
{
    fb_message_t *msg = fb_store_current();
    fb_store_begin(entry->id);

    ESP_LOGD(TAG, "deliver %ld: start", (long)entry->id);

    if (entry->has_photo) {
        size_t len = 0;
        ESP_LOGD(TAG, "deliver: fetching photo");
        if (fb_net_fetch(entry->photo_path, msg->photo, FB_PHOTO_BYTES, &len) == ESP_OK &&
            len == FB_PHOTO_BYTES) {
            msg->photo_len = len;
            msg->has_photo = true;
            ESP_LOGD(TAG, "deliver: photo fetched (%u bytes)", (unsigned)len);
        } else {
            ESP_LOGE(TAG, "photo download failed or wrong size (%u bytes)", (unsigned)len);
        }
    }

    if (entry->has_audio) {
        size_t samples = 0;
        if (fb_net_fetch_wav(entry->audio_path, msg->audio, FB_PLAY_SAMPLES,
                             &samples) == ESP_OK) {
            msg->audio_samples = samples;
            msg->has_audio = true;
        } else {
            ESP_LOGE(TAG, "audio download failed");
        }
    }

    if (!msg->has_photo && !msg->has_audio) {
        /* Nothing usable arrived. Advance the watermark anyway so we do not
         * spin on a broken message forever. */
        fb_store_set_watermark(entry->id);
        return;
    }

    s_busy = true;
    if (msg->has_photo) {
        ESP_LOGD(TAG, "deliver: showing photo");
        fb_ui_show_photo(msg->photo);
        ESP_LOGD(TAG, "deliver: photo shown");
    }

    ESP_LOGD(TAG, "deliver: chime");
    fb_audio_chime_incoming();
    ESP_LOGD(TAG, "deliver: chime done");

    /* Announce arrival, but do not play the note. She decides when to hear
     * it, and can replay it as often as she likes. */
    fb_ui_set_has_audio(msg->has_audio);
    fb_ui_set_unplayed(msg->has_audio);

    fb_ui_set_state(FB_UI_IDLE);
    s_busy = false;
    fb_store_set_watermark(entry->id);
    ESP_LOGI(TAG, "delivered message %ld", (long)entry->id);
}

static void poll_task(void *arg)
{
    LV_UNUSED(arg);
    const TickType_t interval = pdMS_TO_TICKS(CONFIG_FB_POLL_SECONDS * 1000);

    while (true) {
        fb_ui_set_online(fb_net_online());

        ESP_LOGD(TAG, "poll: online=%d busy=%d recording=%d watermark=%ld",
                 (int)fb_net_online(), (int)s_busy, (int)s_recording,
                 (long)fb_store_watermark());

        if (fb_net_online() && !s_busy && !s_recording) {
            fb_inbox_entry_t entry;
            esp_err_t err = fb_net_poll_next(fb_store_watermark(), &entry);
            ESP_LOGD(TAG, "poll result: %s", esp_err_to_name(err));
            if (err == ESP_OK) {
                deliver(&entry);
                continue;   /* drain a backlog without waiting a full interval */
            }
            if (err != ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "poll failed: %s", esp_err_to_name(err));
            }
        }

        vTaskDelay(interval);
    }
}

/* ------------------------------------------------------------- reply side */

static void record_and_queue(void)
{
    /* Claim a slot first: if all three are still waiting to upload, tell her
     * now rather than recording a note we would have to throw away. */
    int slot = fb_store_reply_acquire();
    if (slot < 0) {
        ESP_LOGW(TAG, "all %d reply slots busy; cannot record", FB_REPLY_SLOTS);
        fb_ui_set_state(FB_UI_ERROR);
        fb_audio_chime_error();
        vTaskDelay(pdMS_TO_TICKS(1500));
        fb_ui_set_state(FB_UI_IDLE);
        return;
    }

    int16_t *buf = fb_store_reply_buf(slot);
    const size_t capacity = fb_store_record_capacity();

    s_busy = true;
    s_recording = true;
    fb_ui_set_record_progress(0.0f);
    fb_ui_set_state(FB_UI_RECORDING);
    fb_audio_chime_start_record();

    size_t total = 0;
    while (s_recording && total < capacity) {
        size_t want = capacity - total;
        if (want > RECORD_SLICE_SAMPLES) {
            want = RECORD_SLICE_SAMPLES;
        }
        size_t got = 0;
        if (fb_audio_record(buf + total, want, NULL, &got) != ESP_OK) {
            break;
        }
        total += got;
        fb_ui_set_record_progress((float)total / (float)capacity);
    }
    s_recording = false;

    /* A stray tap produces a fraction of a second of nothing. Silently drop
     * it rather than pushing a notification to a parent in a meeting. */
    if (total < (size_t)FB_SAMPLE_RATE / 2) {
        ESP_LOGI(TAG, "discarding %u-sample tap", (unsigned)total);
        fb_store_reply_release(slot);
        fb_ui_set_state(FB_UI_IDLE);
        s_busy = false;
        return;
    }

    const fb_pending_t pending = {.slot = slot, .samples = total};
    if (xQueueSend(s_upload_q, &pending, 0) != pdTRUE) {
        ESP_LOGE(TAG, "upload queue full; dropping note");
        fb_store_reply_release(slot);
        fb_ui_set_state(FB_UI_ERROR);
        fb_audio_chime_error();
    } else {
        /* Confirmed to her immediately. The upload happens in the background
         * so she can record the next one straight away. */
        fb_ui_set_state(FB_UI_SENT);
        fb_audio_chime_sent();
        fb_ui_set_pending(fb_store_reply_pending());
    }

    vTaskDelay(pdMS_TO_TICKS(1200));
    fb_ui_set_state(FB_UI_IDLE);
    s_busy = false;
}

/* Uploads run here, off the recording path entirely. */
static void upload_task(void *arg)
{
    LV_UNUSED(arg);
    fb_pending_t p;
    while (true) {
        if (xQueueReceive(s_upload_q, &p, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const int16_t *buf = fb_store_reply_buf(p.slot);

        esp_err_t err = ESP_FAIL;
        for (int attempt = 1; attempt <= 5 && err != ESP_OK; attempt++) {
            if (!fb_net_online()) {
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            err = fb_net_send_reply(buf, p.samples);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "reply upload attempt %d failed: %s",
                         attempt, esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(3000 * attempt));
            }
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "giving up on reply after 5 attempts");
        }

        fb_store_reply_release(p.slot);
        fb_ui_set_pending(fb_store_reply_pending());
    }
}

/* The BOOT button (GPIO0, active low). The PWR button is wired to the
 * AXP2101 PMIC rather than the SoC, so it is the hardware power key and is
 * not readable here without the PMU driver. */
#define FB_BOOT_BUTTON GPIO_NUM_0

static void play_current_audio(void)
{
    fb_message_t *msg = fb_store_current();
    if (!msg->has_audio) {
        fb_audio_chime_error();
        return;
    }
    s_busy = true;
    fb_ui_set_unplayed(false);
    fb_ui_set_state(FB_UI_PLAYING);
    fb_audio_play(msg->audio, msg->audio_samples, NULL);
    fb_ui_set_state(FB_UI_IDLE);
    s_busy = false;
}

static void play_task(void *arg)
{
    LV_UNUSED(arg);
    while (true) {
        if (xSemaphoreTake(s_play, portMAX_DELAY) == pdTRUE) {
            play_current_audio();
        }
    }
}

static void boot_button_task(void *arg)
{
    LV_UNUSED(arg);
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << FB_BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    /* Short press replays the note; holding shows the battery. A hold is
     * used rather than a double-press because detecting a double would mean
     * delaying every single press by the double-click window, which would
     * make the common action feel sluggish. */
    const int HOLD_MS = 1200;
    bool was_down = false, hold_fired = false;
    int64_t down_at = 0;

    while (true) {
        const bool down = gpio_get_level(FB_BOOT_BUTTON) == 0;
        const int64_t now = esp_timer_get_time() / 1000;

        if (down && !was_down) {
            down_at = now;
            hold_fired = false;
        } else if (down && !hold_fired && (now - down_at) >= HOLD_MS) {
            hold_fired = true;
            fb_power_t p;
            if (fb_power_read(&p) == ESP_OK) {
                ESP_LOGI(TAG, "BOOT held: battery %d%% charging=%d vbus=%d",
                         p.percent, (int)p.charging, (int)p.vbus);
                fb_ui_show_battery(p.percent, p.charging);
            } else {
                ESP_LOGW(TAG, "battery read failed");
                fb_audio_chime_error();
            }
        } else if (!down && was_down && !hold_fired) {
            if (!s_busy && !s_recording) {
                ESP_LOGI(TAG, "BOOT pressed: replaying last note");
                play_current_audio();
            }
        }

        was_down = down;
        vTaskDelay(pdMS_TO_TICKS(40));   /* also debounces */
    }
}

/* The AMOLED stops being lit after a period of inactivity even though draws
 * keep succeeding and LVGL stays healthy. Root cause still unknown; this
 * re-asserts the panel state periodically so the device stays usable. */
static void display_kick_task(void *arg)
{
    LV_UNUSED(arg);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        fb_ui_kick_display();
    }
}

static void record_task(void *arg)
{
    LV_UNUSED(arg);
    while (true) {
        if (xSemaphoreTake(s_tap, portMAX_DELAY) == pdTRUE) {
            record_and_queue();
        }
    }
}

/* ------------------------------------------------------------------- main */

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(bsp_i2c_init());
    if (fb_power_init() != ESP_OK) {
        ESP_LOGW(TAG, "battery telemetry unavailable");
    }
    ESP_ERROR_CHECK(fb_store_init());

    s_tap = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(s_tap ? ESP_OK : ESP_ERR_NO_MEM);
    s_play = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(s_play ? ESP_OK : ESP_ERR_NO_MEM);
    s_upload_q = xQueueCreate(FB_REPLY_SLOTS, sizeof(fb_pending_t));
    ESP_ERROR_CHECK(s_upload_q ? ESP_OK : ESP_ERR_NO_MEM);

    /* Ordering here is empirical and fragile. What is known to work is:
     * display first, THEN audio after a pause. Both alternatives produce a
     * black panel that still accepts draws without error -
     *   audio before display          -> black
     *   display then audio immediately -> black
     *   display, pause, then audio     -> works
     * The pause is a workaround for a race I have not yet identified, not
     * a understood fix. Do not "tidy" this ordering away. */
    ESP_ERROR_CHECK(fb_ui_init(on_play_tap, on_record_tap));
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_ERROR_CHECK(fb_audio_init());

    ESP_ERROR_CHECK(fb_net_start());

    fb_ui_set_state(FB_UI_IDLE);

    ESP_LOGI(TAG, "heap after init: dma-internal free=%u largest=%u | internal free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    xTaskCreate(poll_task, "fb_poll", 8192, NULL, 5, NULL);
    xTaskCreate(record_task, "fb_record", 8192, NULL, 5, NULL);
    xTaskCreate(boot_button_task, "fb_boot_btn", 4096, NULL, 4, NULL);
    xTaskCreate(display_kick_task, "fb_disp_kick", 4096, NULL, 4, NULL);
    xTaskCreate(upload_task, "fb_upload", 8192, NULL, 5, NULL);
    xTaskCreate(play_task, "fb_play", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "familybox up: relay=%s poll=%ds max_record=%ds",
             FB_RELAY_URL, CONFIG_FB_POLL_SECONDS, CONFIG_FB_MAX_RECORD_SECONDS);
}
