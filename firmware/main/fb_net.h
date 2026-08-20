#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    int32_t id;
    bool has_photo;
    bool has_audio;
    char photo_path[96];
    char audio_path[96];
    size_t photo_bytes;
    size_t audio_bytes;
    int32_t audio_ms;
} fb_inbox_entry_t;

esp_err_t fb_net_start(void);

/* True once we hold an IP. The UI uses this to show a "not connected"
 * state rather than pretending everything is fine. */
bool fb_net_online(void);

/* Oldest inbox entry newer than `since`.
 * ESP_ERR_NOT_FOUND when there is nothing new. */
esp_err_t fb_net_poll_next(int32_t since, fb_inbox_entry_t *out);

/* Download a relay path into `buf`. */
esp_err_t fb_net_fetch(const char *path, uint8_t *buf, size_t cap, size_t *out_len);

/* Download a WAV and return only its PCM payload. */
esp_err_t fb_net_fetch_wav(const char *path, int16_t *buf, size_t max_samples,
                           size_t *out_samples);

/* Upload a recorded reply as a WAV body. */
esp_err_t fb_net_send_reply(const int16_t *pcm, size_t samples);
