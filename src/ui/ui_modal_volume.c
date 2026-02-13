#include "ui_priv.h"
#include "esp_log.h"

#include "watch_settings.h"
#include "watch_volume.h"

static const char *UI_VOL_TAG = "UI_VOL";

static lv_obj_t *volume_modal = NULL;
static lv_obj_t *s_slider     = NULL;

static lv_obj_t *s_req_lbl = NULL;   // "Volume: XX%"
static lv_obj_t *s_gain_lbl = NULL;  // "Gain: 0.42"
static lv_obj_t *s_mute_btn = NULL;
static lv_obj_t *s_mute_lbl = NULL;

static int  s_user_vol_pct = 30;
static bool s_muted = false;

static int clamp_pct(int v)
{
    if (v < 0) return 0;
    if (v > 100) return 100;
    return v;
}

static void update_labels(void)
{
    s_user_vol_pct = clamp_pct(s_user_vol_pct);

    static char a[32], b[32];
    snprintf(a, sizeof(a), "Volume: %d%%", s_user_vol_pct);

    float g = s_muted ? 0.0f : volume_pct_to_gain(s_user_vol_pct);
    snprintf(b, sizeof(b), "Gain: %.2f%s", (double)g, s_muted ? " (muted)" : "");

    if (s_req_lbl)  lv_label_set_text(s_req_lbl, a);
    if (s_gain_lbl) lv_label_set_text(s_gain_lbl, b);

    if (s_mute_lbl) lv_label_set_text(s_mute_lbl, s_muted ? "Unmute" : "Mute");

    // Optional: visually dim slider when muted
    if (s_slider) {
        if (s_muted) lv_obj_add_state(s_slider, LV_STATE_DISABLED);
        else         lv_obj_clear_state(s_slider, LV_STATE_DISABLED);
    }
}

static void volume_modal_close(lv_event_t *e)
{
    (void)e;
    if (!volume_modal) return;

    // Save what user picked
    settings_save_volume_pct(s_user_vol_pct);
    settings_save_volume_muted(s_muted);

    ESP_LOGI(UI_VOL_TAG, "Saving volume=%d muted=%d", s_user_vol_pct, (int)s_muted);

    lv_obj_del(volume_modal);
    volume_modal = NULL;
    s_slider = NULL;
    s_req_lbl = s_gain_lbl = NULL;
    s_mute_btn = s_mute_lbl = NULL;
}

static void on_volume_changed(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *slider = lv_event_get_target(e);
    s_user_vol_pct = (int)lv_slider_get_value(slider);

    // Apply immediately
    volume_set_user_pct(s_user_vol_pct);

    update_labels();
    mark_user_activity();
}

static void on_mute_clicked(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    s_muted = !s_muted;
    volume_set_muted(s_muted);

    update_labels();
    mark_user_activity();
}

void open_volume_modal(lv_obj_t *parent)
{
    if (volume_modal) return;

    // Initialize local state from current system state
    s_user_vol_pct = volume_get_user_pct();
    s_muted        = volume_get_muted();

    volume_modal = lv_obj_create(parent);
    lv_obj_set_size(volume_modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(volume_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(volume_modal, LV_OPA_70, 0);
    lv_obj_clear_flag(volume_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(volume_modal);
    lv_obj_set_size(card, 340, 220);
    lv_obj_center(card);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, "Volume");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 8);

    s_slider = lv_slider_create(card);
    lv_obj_set_width(s_slider, lv_pct(90));
    lv_obj_align(s_slider, LV_ALIGN_CENTER, 0, 10);

    lv_slider_set_range(s_slider, 0, 100);
    lv_slider_set_value(s_slider, clamp_pct(s_user_vol_pct), LV_ANIM_OFF);
    lv_obj_add_event_cb(s_slider, on_volume_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // Labels at bottom-left
    s_req_lbl = lv_label_create(card);
    lv_obj_align(s_req_lbl, LV_ALIGN_BOTTOM_LEFT, 12, -44);

    s_gain_lbl = lv_label_create(card);
    lv_obj_align(s_gain_lbl, LV_ALIGN_BOTTOM_LEFT, 12, -24);

    // Mute button
    s_mute_btn = lv_btn_create(card);
    lv_obj_set_size(s_mute_btn, 90, 36);
    lv_obj_align(s_mute_btn, LV_ALIGN_BOTTOM_LEFT, 12, -10);
    lv_obj_add_event_cb(s_mute_btn, on_mute_clicked, LV_EVENT_CLICKED, NULL);

    s_mute_lbl = lv_label_create(s_mute_btn);
    lv_obj_center(s_mute_lbl);

    // Save button
    lv_obj_t *save = lv_btn_create(card);
    lv_obj_set_size(save, 90, 36);
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, -12, -10);
    lv_obj_add_event_cb(save, volume_modal_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save_lbl = lv_label_create(save);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_center(save_lbl);

    update_labels();
}