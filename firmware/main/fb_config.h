#pragma once

#include <stddef.h>
#include <stdint.h>

/* Panel geometry. The relay pre-crops every photo to exactly this, so the
 * firmware never scales or decodes anything. */
#define FB_PANEL_W 368
#define FB_PANEL_H 448
#define FB_PHOTO_BYTES ((size_t)FB_PANEL_W * FB_PANEL_H * 2)

/* The ES8311's native rate, used unchanged in both directions. */
#define FB_SAMPLE_RATE 16000
#define FB_CHANNELS 1

#define FB_MAX_PLAY_SECONDS 30
#define FB_PLAY_SAMPLES ((size_t)FB_SAMPLE_RATE * FB_MAX_PLAY_SECONDS)
#define FB_RECORD_SAMPLES ((size_t)FB_SAMPLE_RATE * CONFIG_FB_MAX_RECORD_SECONDS)

/* She can stack up several voice notes before any of them finish uploading,
 * and before a new photo arrives. Each slot holds one full-length note. */
#define FB_REPLY_SLOTS 3

#define FB_RELAY_URL CONFIG_FB_RELAY_URL
#define FB_RELAY_TOKEN CONFIG_FB_RELAY_TOKEN
