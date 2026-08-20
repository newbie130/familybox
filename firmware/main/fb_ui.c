#include "fb_ui.h"

/* esp_lcd_touch.h must precede bsp/touch.h, which uses its handle type. */
#include "esp_lcd_touch.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_touch.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_check.h"
#include "esp_log.h"
#include "fb_config.h"
#include "lvgl.h"

static const char *TAG = "fb_ui";

#define BUTTON_DIAMETER 122
#define BUTTON_OFFSET_X 88     /* from centre, so the pair straddles the middle */
#define BUTTON_OFFSET_Y (-34)

static lv_display_t *s_disp;
static esp_lcd_panel_handle_t s_panel;
static lv_obj_t *s_photo;
static lv_obj_t *s_play_btn;
static lv_obj_t *s_rec_btn;
static lv_obj_t *s_rec_icon;
static lv_obj_t *s_rec_dot;
static lv_obj_t *s_ring;
static lv_obj_t *s_offline_dot;
static lv_obj_t *s_idle_face;
static lv_obj_t *s_pending_dots[FB_REPLY_SLOTS];
static lv_obj_t *s_batt_box;
static lv_obj_t *s_batt_fill;
static lv_obj_t *s_batt_text;
static lv_obj_t *s_batt_bolt;
static lv_timer_t *s_batt_timer;
static lv_image_dsc_t s_photo_dsc;
static fb_ui_tap_cb_t s_on_play;
static fb_ui_tap_cb_t s_on_record;

/* Saturated and high contrast: read across a dim bedroom by someone who
 * cannot fall back on reading the label. */
static const lv_color_t COLOR_PLAY = LV_COLOR_MAKE(0x2E, 0xC4, 0x6B);   /* green */
static const lv_color_t COLOR_RECORD = LV_COLOR_MAKE(0xE5, 0x3E, 0x3E); /* red   */
static const lv_color_t COLOR_BUSY = LV_COLOR_MAKE(0xF5, 0xA6, 0x23);   /* amber */
static const lv_color_t COLOR_OFF = LV_COLOR_MAKE(0x60, 0x60, 0x60);

/* An animation callback is exactly (void *, int32_t). Casting a 3-argument
 * style setter to it passes a garbage selector every frame and wedges the
 * LVGL task while it holds its mutex. Always wrap. */
static void opa_anim_cb(void *obj, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void on_play_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_on_play) {
        s_on_play();
    }
}

static void on_record_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_on_record) {
        s_on_record();
    }
}

/* A camera-style record dot: a filled circle for "record", morphing to a
 * rounded square for "stop". Both shapes are universally understood, need no
 * text, and read cleanly at any size - unlike the microphone this replaced,
 * which was assembled from rectangles and an arc and looked it. */
static lv_obj_t *make_record_dot(lv_obj_t *parent)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 56, 56);
    lv_obj_center(d);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
    return d;
}

static void set_dot_recording(bool recording)
{
    if (recording) {
        lv_obj_set_size(s_rec_dot, 44, 44);
        lv_obj_set_style_radius(s_rec_dot, 8, 0);
    } else {
        lv_obj_set_size(s_rec_dot, 56, 56);
        lv_obj_set_style_radius(s_rec_dot, LV_RADIUS_CIRCLE, 0);
    }
    lv_obj_center(s_rec_dot);
}

static void style_round_button(lv_obj_t *btn, lv_color_t color)
{
    lv_obj_set_size(btn, BUTTON_DIAMETER, BUTTON_DIAMETER);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_border_width(btn, 5, 0);
    lv_obj_set_style_border_color(btn, lv_color_white(), 0);
    lv_obj_set_style_shadow_width(btn, 24, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_50, 0);
}

/* The BSP's bsp_display_start() wires LVGL through lvgl_port_add_disp_rgb(),
 * which is for panels on the RGB/parallel interface. This board's CO5300 is
 * QSPI, so that path renders happily and flushes nothing - a black screen with
 * no errors logged. Wiring lvgl_port_add_disp() directly is the supported path
 * and also lets us demand a DMA-capable draw buffer. */
static esp_err_t display_init(void)
{
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl port init failed");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(bsp_display_new(NULL, &panel, &io), TAG, "panel init failed");
    s_panel = panel;

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = FB_PANEL_W * 40,
        .double_buffer = false,
        .hres = FB_PANEL_W,
        .vres = FB_PANEL_H,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .flags = {.buff_dma = true, .buff_spiram = false, .swap_bytes = true},
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "lvgl add display failed");

    esp_lcd_touch_handle_t tp = NULL;
    if (bsp_touch_new(NULL, &tp) == ESP_OK) {
        const lvgl_port_touch_cfg_t touch_cfg = {.disp = s_disp, .handle = tp};
        if (!lvgl_port_add_touch(&touch_cfg)) {
            ESP_LOGW(TAG, "touch not registered; buttons will be unusable");
        }
    } else {
        ESP_LOGW(TAG, "touch init failed");
    }

    bsp_display_brightness_init();
    bsp_display_brightness_set(100);
    return ESP_OK;
}

esp_err_t fb_ui_init(fb_ui_tap_cb_t on_play, fb_ui_tap_cb_t on_record)
{
    s_on_play = on_play;
    s_on_record = on_record;

    ESP_RETURN_ON_ERROR(display_init(), TAG, "display init failed");
    bsp_display_lock(0);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    s_photo_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_photo_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_photo_dsc.header.w = FB_PANEL_W;
    s_photo_dsc.header.h = FB_PANEL_H;
    s_photo_dsc.header.stride = FB_PANEL_W * 2;
    s_photo_dsc.data_size = FB_PHOTO_BYTES;
    s_photo_dsc.data = NULL;

    s_photo = lv_image_create(screen);
    lv_obj_set_pos(s_photo, 0, 0);
    lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);

    /* Sleeping face for "awake, nothing new". A closed-eyed smile reads as
     * asleep to a child without any words, and is clearly not a dead screen. */
    s_idle_face = lv_obj_create(screen);
    lv_obj_set_size(s_idle_face, 152, 152);
    lv_obj_align(s_idle_face, LV_ALIGN_CENTER, 0, -78);
    lv_obj_set_style_radius(s_idle_face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_idle_face, lv_color_hex(0x3B4A6B), 0);
    lv_obj_set_style_border_width(s_idle_face, 0, 0);
    lv_obj_remove_flag(s_idle_face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_idle_face, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < 2; i++) {
        lv_obj_t *eye = lv_obj_create(s_idle_face);
        lv_obj_set_size(eye, 30, 7);
        lv_obj_align(eye, LV_ALIGN_CENTER, i == 0 ? -32 : 32, -18);
        lv_obj_set_style_radius(eye, 4, 0);
        lv_obj_set_style_bg_color(eye, lv_color_white(), 0);
        lv_obj_set_style_border_width(eye, 0, 0);
        lv_obj_remove_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_t *mouth = lv_arc_create(s_idle_face);
    lv_obj_set_size(mouth, 66, 66);
    lv_obj_align(mouth, LV_ALIGN_CENTER, 0, 24);
    lv_arc_set_bg_angles(mouth, 30, 150);
    lv_obj_remove_style(mouth, NULL, LV_PART_KNOB);
    lv_obj_remove_style(mouth, NULL, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(mouth, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_color(mouth, lv_color_white(), LV_PART_MAIN);
    lv_obj_remove_flag(mouth, LV_OBJ_FLAG_CLICKABLE);

    lv_anim_t breathe;
    lv_anim_init(&breathe);
    lv_anim_set_var(&breathe, s_idle_face);
    lv_anim_set_values(&breathe, LV_OPA_50, LV_OPA_COVER);
    lv_anim_set_duration(&breathe, 2200);
    lv_anim_set_playback_duration(&breathe, 2200);
    lv_anim_set_repeat_count(&breathe, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&breathe, opa_anim_cb);
    lv_anim_start(&breathe);

    /* Countdown ring sits behind the record button as a halo. */
    s_ring = lv_arc_create(screen);
    lv_obj_set_size(s_ring, BUTTON_DIAMETER + 26, BUTTON_DIAMETER + 26);
    lv_obj_align(s_ring, LV_ALIGN_BOTTOM_MID, BUTTON_OFFSET_X, BUTTON_OFFSET_Y + 13);
    lv_arc_set_rotation(s_ring, 270);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_value(s_ring, 0);
    lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_ring, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, COLOR_RECORD, LV_PART_INDICATOR);
    lv_obj_add_flag(s_ring, LV_OBJ_FLAG_HIDDEN);

    /* Left: green speaker, plays Dad's note. Right: red microphone, records
     * hers. Fixed positions so muscle memory works before reading does. */
    s_play_btn = lv_button_create(screen);
    style_round_button(s_play_btn, COLOR_PLAY);
    lv_obj_align(s_play_btn, LV_ALIGN_BOTTOM_MID, -BUTTON_OFFSET_X, BUTTON_OFFSET_Y);
    lv_obj_add_event_cb(s_play_btn, on_play_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *speaker = lv_label_create(s_play_btn);
    lv_label_set_text(speaker, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(speaker, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(speaker, lv_color_white(), 0);
    lv_obj_center(speaker);

    s_rec_btn = lv_button_create(screen);
    style_round_button(s_rec_btn, COLOR_RECORD);
    lv_obj_align(s_rec_btn, LV_ALIGN_BOTTOM_MID, BUTTON_OFFSET_X, BUTTON_OFFSET_Y);
    lv_obj_add_event_cb(s_rec_btn, on_record_clicked, LV_EVENT_CLICKED, NULL);

    s_rec_dot = make_record_dot(s_rec_btn);
    s_rec_icon = lv_label_create(s_rec_btn);
    lv_obj_set_style_text_font(s_rec_icon, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_rec_icon, lv_color_white(), 0);
    lv_label_set_text(s_rec_icon, "");
    lv_obj_center(s_rec_icon);
    lv_obj_add_flag(s_rec_icon, LV_OBJ_FLAG_HIDDEN);

    /* One dot per voice note still waiting to upload. */
    for (int i = 0; i < FB_REPLY_SLOTS; i++) {
        s_pending_dots[i] = lv_obj_create(screen);
        lv_obj_set_size(s_pending_dots[i], 13, 13);
        lv_obj_align(s_pending_dots[i], LV_ALIGN_TOP_LEFT, 14 + i * 20, 14);
        lv_obj_set_style_radius(s_pending_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_pending_dots[i], 0, 0);
        lv_obj_set_style_bg_color(s_pending_dots[i], COLOR_BUSY, 0);
        lv_obj_set_style_bg_opa(s_pending_dots[i], LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(s_pending_dots[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Battery overlay: hidden until she long-presses BOOT. A filled bar with
     * colour reads as "how full" to a child; the number is there for adults. */
    s_batt_box = lv_obj_create(screen);
    lv_obj_set_size(s_batt_box, 236, 132);
    lv_obj_center(s_batt_box);
    lv_obj_set_style_radius(s_batt_box, 18, 0);
    lv_obj_set_style_bg_color(s_batt_box, lv_color_hex(0x0b1220), 0);
    lv_obj_set_style_bg_opa(s_batt_box, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_batt_box, 2, 0);
    lv_obj_set_style_border_color(s_batt_box, lv_color_hex(0x2b3a52), 0);
    lv_obj_remove_flag(s_batt_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_batt_box, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *shell = lv_obj_create(s_batt_box);
    lv_obj_set_size(shell, 140, 62);
    lv_obj_align(shell, LV_ALIGN_TOP_MID, -6, 2);
    lv_obj_set_style_radius(shell, 10, 0);
    lv_obj_set_style_bg_opa(shell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(shell, 5, 0);
    lv_obj_set_style_border_color(shell, lv_color_white(), 0);
    lv_obj_set_style_pad_all(shell, 6, 0);
    lv_obj_remove_flag(shell, LV_OBJ_FLAG_SCROLLABLE);

    s_batt_fill = lv_obj_create(shell);
    lv_obj_set_size(s_batt_fill, 60, 40);
    lv_obj_align(s_batt_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(s_batt_fill, 5, 0);
    lv_obj_set_style_border_width(s_batt_fill, 0, 0);
    lv_obj_set_style_bg_color(s_batt_fill, COLOR_PLAY, 0);
    lv_obj_remove_flag(s_batt_fill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *nub = lv_obj_create(s_batt_box);
    lv_obj_set_size(nub, 9, 24);
    lv_obj_align(nub, LV_ALIGN_TOP_MID, 70, 21);
    lv_obj_set_style_radius(nub, 3, 0);
    lv_obj_set_style_border_width(nub, 0, 0);
    lv_obj_set_style_bg_color(nub, lv_color_white(), 0);
    lv_obj_remove_flag(nub, LV_OBJ_FLAG_SCROLLABLE);

    s_batt_bolt = lv_label_create(s_batt_box);
    lv_label_set_text(s_batt_bolt, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_font(s_batt_bolt, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_batt_bolt, lv_color_hex(0xF5A623), 0);
    lv_obj_align(s_batt_bolt, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_add_flag(s_batt_bolt, LV_OBJ_FLAG_HIDDEN);

    s_batt_text = lv_label_create(s_batt_box);
    lv_obj_set_style_text_font(s_batt_text, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_batt_text, lv_color_white(), 0);
    lv_obj_align(s_batt_text, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_label_set_text(s_batt_text, "");

    s_offline_dot = lv_obj_create(screen);
    lv_obj_set_size(s_offline_dot, 16, 16);
    lv_obj_align(s_offline_dot, LV_ALIGN_TOP_RIGHT, -14, 14);
    lv_obj_set_style_radius(s_offline_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_offline_dot, 0, 0);
    lv_obj_set_style_bg_color(s_offline_dot, lv_color_hex(0x808080), 0);
    lv_obj_remove_flag(s_offline_dot, LV_OBJ_FLAG_SCROLLABLE);

    bsp_display_unlock();
    ESP_LOGI(TAG, "ui ready (%dx%d)", FB_PANEL_W, FB_PANEL_H);
    return ESP_OK;
}

static void bring_controls_forward(void)
{
    lv_obj_move_foreground(s_ring);
    lv_obj_move_foreground(s_play_btn);
    lv_obj_move_foreground(s_rec_btn);
    lv_obj_move_foreground(s_offline_dot);
    for (int i = 0; i < FB_REPLY_SLOTS; i++) {
        lv_obj_move_foreground(s_pending_dots[i]);
    }
}

void fb_ui_show_photo(const uint8_t *rgb565)
{
    if (!bsp_display_lock(3000)) {
        ESP_LOGE(TAG, "display lock timeout; cannot show photo");
        return;
    }
    s_photo_dsc.data = rgb565;
    lv_image_set_src(s_photo, &s_photo_dsc);
    lv_obj_remove_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
    if (s_idle_face) {
        lv_anim_delete(s_idle_face, NULL);
        lv_obj_add_flag(s_idle_face, LV_OBJ_FLAG_HIDDEN);
    }
    bring_controls_forward();
    lv_obj_invalidate(s_photo);
    bsp_display_unlock();
}

void fb_ui_set_has_audio(bool has_audio)
{
    if (!bsp_display_lock(1000)) {
        return;
    }
    if (has_audio) {
        lv_obj_remove_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);
    }
    bsp_display_unlock();
}

void fb_ui_set_unplayed(bool unplayed)
{
    if (!bsp_display_lock(1000)) {
        return;
    }
    lv_anim_delete(s_play_btn, NULL);
    if (unplayed) {
        /* Pulse until she plays it, so a new note invites a press rather
         * than sitting there looking identical to an old one. */
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_play_btn);
        lv_anim_set_values(&a, LV_OPA_60, LV_OPA_COVER);
        lv_anim_set_duration(&a, 700);
        lv_anim_set_playback_duration(&a, 700);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&a, opa_anim_cb);
        lv_anim_start(&a);
    } else {
        lv_obj_set_style_opa(s_play_btn, LV_OPA_COVER, 0);
    }
    bsp_display_unlock();
}

void fb_ui_set_state(fb_ui_state_t state)
{
    if (!bsp_display_lock(1000)) {
        return;
    }

    lv_obj_add_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rec_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN);
    set_dot_recording(false);
    lv_obj_add_flag(s_rec_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_play_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_rec_btn, COLOR_RECORD, 0);

    switch (state) {
    case FB_UI_IDLE:
        break;
    case FB_UI_PLAYING:
        /* Recording is blocked mid-playback: otherwise she records over the
         * top of Dad still talking. */
        lv_obj_remove_flag(s_rec_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(s_rec_btn, COLOR_OFF, 0);
        break;
    case FB_UI_RECORDING:
        lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_HIDDEN);
        set_dot_recording(true);          /* circle becomes a stop square */
        lv_obj_remove_flag(s_play_btn, LV_OBJ_FLAG_CLICKABLE);
        break;
    case FB_UI_SENDING:
        lv_obj_add_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_rec_icon, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_rec_icon, LV_SYMBOL_UPLOAD);
        lv_obj_set_style_bg_color(s_rec_btn, COLOR_BUSY, 0);
        lv_obj_remove_flag(s_rec_btn, LV_OBJ_FLAG_CLICKABLE);
        break;
    case FB_UI_SENT:
        lv_obj_add_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_rec_icon, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_rec_icon, LV_SYMBOL_OK);
        lv_obj_set_style_bg_color(s_rec_btn, COLOR_PLAY, 0);
        lv_obj_remove_flag(s_rec_btn, LV_OBJ_FLAG_CLICKABLE);
        break;
    case FB_UI_ERROR:
        lv_obj_add_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_rec_icon, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_rec_icon, LV_SYMBOL_CLOSE);
        lv_obj_set_style_bg_color(s_rec_btn, COLOR_OFF, 0);
        break;
    }

    bsp_display_unlock();
}

void fb_ui_set_record_progress(float fraction)
{
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;
    if (!bsp_display_lock(200)) {
        return;
    }
    /* Counts down, so the ring empties as her time runs out. */
    lv_arc_set_value(s_ring, (int32_t)((1.0f - fraction) * 100.0f));
    bsp_display_unlock();
}

void fb_ui_set_online(bool online)
{
    static int last = -1;
    if (last == (int)online) {
        return;
    }
    if (!bsp_display_lock(200)) {
        ESP_LOGW(TAG, "display busy; skipping online indicator update");
        return;
    }
    last = (int)online;
    lv_obj_set_style_bg_opa(s_offline_dot, online ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    bsp_display_unlock();
}

void fb_ui_set_pending(int count)
{
    if (!bsp_display_lock(200)) {
        return;
    }
    for (int i = 0; i < FB_REPLY_SLOTS; i++) {
        lv_obj_set_style_bg_opa(s_pending_dots[i],
                                i < count ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    bsp_display_unlock();
}

void fb_ui_kick_display(void)
{
    /* Must hold the LVGL lock: these are SPI transactions on the same bus
     * LVGL flushes through. An earlier version ran this inside an LVGL timer,
     * which already holds the lock, and deadlocked the LVGL task. */
    if (!bsp_display_lock(500)) {
        ESP_LOGW(TAG, "display busy; skipping panel refresh");
        return;
    }
    if (s_panel) {
        esp_lcd_panel_disp_on_off(s_panel, true);
    }
    bsp_display_brightness_set(100);
    lv_obj_invalidate(lv_screen_active());
    bsp_display_unlock();
}

static void batt_hide_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    lv_obj_add_flag(s_batt_box, LV_OBJ_FLAG_HIDDEN);
    s_batt_timer = NULL;
}

void fb_ui_show_battery(int percent, bool charging)
{
    if (!bsp_display_lock(1000)) {
        return;
    }

    char text[24];
    if (percent < 0) {
        lv_obj_set_width(s_batt_fill, 0);
        snprintf(text, sizeof(text), "USB");
    } else {
        /* Inner width is 140 minus the 5px border and 6px padding each side. */
        const int inner = 140 - (5 + 6) * 2;
        int w = inner * percent / 100;
        lv_obj_set_width(s_batt_fill, w < 4 ? 4 : w);
        lv_obj_set_style_bg_color(s_batt_fill,
                                  percent <= 15 ? COLOR_RECORD :
                                  percent <= 35 ? COLOR_BUSY : COLOR_PLAY, 0);
        snprintf(text, sizeof(text), "%d%%", percent);
    }
    lv_label_set_text(s_batt_text, text);

    if (charging) {
        lv_obj_remove_flag(s_batt_bolt, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_batt_bolt, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_move_foreground(s_batt_box);
    lv_obj_remove_flag(s_batt_box, LV_OBJ_FLAG_HIDDEN);

    if (s_batt_timer) {
        lv_timer_delete(s_batt_timer);
    }
    s_batt_timer = lv_timer_create(batt_hide_cb, 4000, NULL);
    lv_timer_set_repeat_count(s_batt_timer, 1);

    bsp_display_unlock();
}
