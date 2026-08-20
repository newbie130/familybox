#include "fb_audio.h"

#include <math.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fb_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "fb_audio";

#define CHUNK_SAMPLES 512

static esp_codec_dev_handle_t s_speaker;
static esp_codec_dev_handle_t s_mic;
static int16_t *s_chunk;

static esp_codec_dev_sample_info_t sample_info(void)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = FB_CHANNELS,
        .channel_mask = 0,
        .sample_rate = FB_SAMPLE_RATE,
        .mclk_multiple = 256,
    };
    return fs;
}

esp_err_t fb_audio_init_i2s(void)
{
    const i2s_std_config_t i2s_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(FB_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = {false, false, false},
        },
    };
    ESP_RETURN_ON_ERROR(bsp_audio_init(&i2s_cfg), TAG, "bsp audio init failed");

    if (!s_chunk) {
        s_chunk = heap_caps_malloc(CHUNK_SAMPLES * sizeof(int16_t),
                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        ESP_RETURN_ON_FALSE(s_chunk, ESP_ERR_NO_MEM, TAG, "chunk buffer alloc failed");
    }
    ESP_LOGI(TAG, "stage: i2s ready");
    return ESP_OK;
}

esp_err_t fb_audio_init_speaker(void)
{
    s_speaker = bsp_audio_codec_speaker_init();
    ESP_RETURN_ON_FALSE(s_speaker, ESP_FAIL, TAG, "speaker init failed");
    esp_codec_dev_sample_info_t fs = sample_info();
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_speaker, &fs) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "speaker open failed");
    esp_codec_dev_set_out_vol(s_speaker, 80);
    ESP_LOGI(TAG, "stage: speaker ready");
    return ESP_OK;
}

esp_err_t fb_audio_init_mic(void)
{
    s_mic = bsp_audio_codec_microphone_init();
    ESP_RETURN_ON_FALSE(s_mic, ESP_FAIL, TAG, "microphone init failed");
    esp_codec_dev_sample_info_t fs = sample_info();
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_mic, &fs) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "microphone open failed");
    esp_codec_dev_set_in_gain(s_mic, 30.0);
    ESP_LOGI(TAG, "stage: mic ready");
    return ESP_OK;
}

esp_err_t fb_audio_init(void)
{
    ESP_RETURN_ON_ERROR(fb_audio_init_i2s(), TAG, "i2s stage failed");
    ESP_RETURN_ON_ERROR(fb_audio_init_speaker(), TAG, "speaker stage failed");
    ESP_RETURN_ON_ERROR(fb_audio_init_mic(), TAG, "mic stage failed");
    ESP_LOGI(TAG, "codec ready: %d Hz mono, speaker + mic open", FB_SAMPLE_RATE);
    return ESP_OK;
}

void fb_audio_set_volume(int percent)
{
    if (s_speaker) {
        esp_codec_dev_set_out_vol(s_speaker, percent);
    }
}

esp_err_t fb_audio_play(const int16_t *pcm, size_t samples, const volatile bool *abort)
{
    ESP_RETURN_ON_FALSE(s_speaker && s_chunk, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    size_t done = 0;
    while (done < samples) {
        if (abort && *abort) {
            break;
        }
        size_t n = (samples - done > CHUNK_SAMPLES) ? CHUNK_SAMPLES : samples - done;
        /* Staged through internal RAM because the source lives in PSRAM. */
        memcpy(s_chunk, pcm + done, n * sizeof(int16_t));
        int ret = esp_codec_dev_write(s_speaker, s_chunk, (int)(n * sizeof(int16_t)));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "playback write failed: %d", ret);
            return ESP_FAIL;
        }
        done += n;
    }
    return ESP_OK;
}

esp_err_t fb_audio_record(int16_t *buf, size_t max_samples, const volatile bool *stop,
                          size_t *out_samples)
{
    ESP_RETURN_ON_FALSE(s_mic && s_chunk, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    size_t done = 0;
    while (done < max_samples) {
        if (stop && *stop) {
            break;
        }
        size_t n = (max_samples - done > CHUNK_SAMPLES) ? CHUNK_SAMPLES : max_samples - done;
        int ret = esp_codec_dev_read(s_mic, s_chunk, (int)(n * sizeof(int16_t)));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "capture read failed: %d", ret);
            return ESP_FAIL;
        }
        memcpy(buf + done, s_chunk, n * sizeof(int16_t));
        done += n;
    }

    *out_samples = done;
    ESP_LOGI(TAG, "recorded %u samples (%.1f s)", (unsigned)done,
             (double)done / FB_SAMPLE_RATE);
    return ESP_OK;
}

/* ----------------------------------------------------------------- tones */

static void tone(int freq_hz, int ms, float amplitude)
{
    if (!s_speaker || !s_chunk) {
        return;
    }
    const size_t total = (size_t)FB_SAMPLE_RATE * ms / 1000;
    const float step = 2.0f * (float)M_PI * freq_hz / FB_SAMPLE_RATE;
    float phase = 0.0f;
    size_t done = 0;

    while (done < total) {
        size_t n = (total - done > CHUNK_SAMPLES) ? CHUNK_SAMPLES : total - done;
        for (size_t i = 0; i < n; i++) {
            /* Fade the last 5 ms so the tone stops without a click. */
            size_t remaining = total - done - i;
            float env = (remaining < 80) ? (float)remaining / 80.0f : 1.0f;
            s_chunk[i] = (int16_t)(sinf(phase) * 12000.0f * amplitude * env);
            phase += step;
            if (phase >= 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
        }
        esp_codec_dev_write(s_speaker, s_chunk, (int)(n * sizeof(int16_t)));
        done += n;
    }
}

void fb_audio_chime_incoming(void)
{
    tone(784, 120, 0.8f);   /* G5 */
    tone(1047, 180, 0.8f);  /* C6 - rising, "something arrived" */
}

void fb_audio_chime_start_record(void)
{
    tone(880, 100, 0.7f);
}

void fb_audio_chime_sent(void)
{
    tone(1047, 90, 0.7f);
    tone(1319, 140, 0.7f);
}

void fb_audio_chime_error(void)
{
    tone(330, 200, 0.6f);   /* low and flat, clearly not a success sound */
}
