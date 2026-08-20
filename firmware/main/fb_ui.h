#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    FB_UI_IDLE,        /* nothing happening; both buttons available */
    FB_UI_PLAYING,     /* playing Dad's note */
    FB_UI_RECORDING,   /* capturing her reply */
    FB_UI_SENDING,
    FB_UI_SENT,
    FB_UI_ERROR,
} fb_ui_state_t;

/* Fired from the LVGL task, so these must not block. */
typedef void (*fb_ui_tap_cb_t)(void);

esp_err_t fb_ui_init(fb_ui_tap_cb_t on_play, fb_ui_tap_cb_t on_record);

/* Point the on-screen image at a PSRAM RGB565 buffer and redraw. */
void fb_ui_show_photo(const uint8_t *rgb565);

/* Show/hide the green play button. Hidden when there is no voice note. */
void fb_ui_set_has_audio(bool has_audio);

/* Draw attention to the play button for a note she has not heard yet. */
void fb_ui_set_unplayed(bool unplayed);

void fb_ui_set_state(fb_ui_state_t state);
void fb_ui_set_record_progress(float fraction);
void fb_ui_set_online(bool online);
void fb_ui_set_pending(int count);
void fb_ui_kick_display(void);

/* Battery overlay, shown briefly on a long-press of BOOT. percent < 0 means
 * no battery is connected (running on USB alone). */
void fb_ui_show_battery(int percent, bool charging);
