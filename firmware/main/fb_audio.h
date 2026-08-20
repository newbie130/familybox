#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t fb_audio_init(void);

/* Staged variants, used to bisect which step disturbs the display. */
esp_err_t fb_audio_init_i2s(void);
esp_err_t fb_audio_init_speaker(void);
esp_err_t fb_audio_init_mic(void);

/* Blocking playback of mono PCM16 at FB_SAMPLE_RATE.
 * Returns early if *abort becomes true; pass NULL to always play through. */
esp_err_t fb_audio_play(const int16_t *pcm, size_t samples, const volatile bool *abort);

/* Blocking capture into `buf`, stopping when *stop becomes true, when the
 * buffer fills, or after CONFIG_FB_MAX_RECORD_SECONDS. */
esp_err_t fb_audio_record(int16_t *buf, size_t max_samples, const volatile bool *stop,
                          size_t *out_samples);

/* Short synthesised tones. A pre-literate user needs to hear that a button
 * did something, and these cost no flash unlike sampled sounds. */
void fb_audio_chime_incoming(void);
void fb_audio_chime_start_record(void);
void fb_audio_chime_sent(void);
void fb_audio_chime_error(void);

void fb_audio_set_volume(int percent);
