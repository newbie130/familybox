#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Storage for the one message currently on screen, plus the reply being
 * recorded. Backed by PSRAM today; the interface exists so an SD-card
 * backend can be dropped in later without touching callers. */

typedef struct {
    int32_t id;
    bool has_photo;
    bool has_audio;
    uint8_t *photo;        /* RGB565 little-endian, FB_PHOTO_BYTES */
    size_t photo_len;
    int16_t *audio;        /* PCM16 mono @ FB_SAMPLE_RATE */
    size_t audio_samples;
} fb_message_t;

esp_err_t fb_store_init(void);

/* The message currently held for display. Never NULL after init. */
fb_message_t *fb_store_current(void);

/* Discard whatever is held and start filling a new message. */
void fb_store_begin(int32_t id);

/* Reply slots. Recording claims one, the uploader releases it when the note
 * has been sent, so recording never has to wait for the network. */
size_t fb_store_record_capacity(void);
int fb_store_reply_acquire(void);          /* slot index, or -1 if all full */
int16_t *fb_store_reply_buf(int slot);
void fb_store_reply_release(int slot);
int fb_store_reply_pending(void);          /* slots currently in use */

/* Watermark of the highest inbox id already shown, persisted in NVS so a
 * reboot does not replay yesterday's messages. */
int32_t fb_store_watermark(void);
esp_err_t fb_store_set_watermark(int32_t id);
