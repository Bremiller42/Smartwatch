// FILE: src/ui/ui_modal_brightness.c
#include "ui_priv.h"
#include "esp_log.h"

#include "watch_backlight.h"
#include "watch_settings.h"

static const char *UI_BRT_TAG = "UI_BRIGHT";

static lv_obj_t *brightness_modal = NULL;
static lv_obj_t *s_slider = NULL;

// Labels to show Requested / Cap / Effective
static lv_obj_t *s_req_lbl = NULL;
static lv_obj_t *s_cap_lbl = NULL;
static lv_obj_t *s_eff_lbl = NULL;

// Track what we intend to save (user preference)
static int s_user_pct = 10;

static int clamp_pct(int v)
{
    if (v < 0) return 0;
    if (v > 100) return 100;
    return v;
}

static void update_labels(void)
{
    int cap = backlight_get_cap_pct();
    cap = clamp_pct(cap);
    if (cap < 10) cap = 10;

    int req = backlight_get_user_pct();
    req = clamp_pct(req);
    if (req < 10) req = 10;

    int eff = backlight_get_effective_pct();
    eff = clamp_pct(eff);
    if (eff < 10) eff = 10;

    static char a[32], b[32], c[32];
    snprintf(a, sizeof(a), "Requested: %d%%", req);
    snprintf(b, sizeof(b), "Cap: %d%%", cap);
    snprintf(c, sizeof(c), "Effective: %d%%", eff);

    if (s_req_lbl) lv_label_set_text(s_req_lbl, a);
    if (s_cap_lbl) lv_label_set_text(s_cap_lbl, b);
    if (s_eff_lbl) lv_label_set_text(s_eff_lbl, c);

    // Optional: warn when capped
    if (cap < req) {
        // You can change this to a toast later
        ESP_LOGI(UI_BRT_TAG, "Brightness is capped (req=%d cap=%d eff=%d)", req, cap, eff);
    }
}

static void brightness_modal_close(lv_event_t *e)
{
    (void)e;

    if (!brightness_modal) return;

    // Always save the actual current user preference (not stale local)
    s_user_pct = backlight_get_user_pct();
    if (s_user_pct < 10) s_user_pct = 10;

    ESP_LOGI(UI_BRT_TAG, "Brightness user=%d -> saving", s_user_pct);
    settings_save_brightness(s_user_pct);

    lv_obj_del(brightness_modal);
    brightness_modal = NULL;
    s_slider = NULL;
    s_req_lbl = s_cap_lbl = s_eff_lbl = NULL;
}

static void on_brightness_changed(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *slider = lv_event_get_target(e);
    s_user_pct = (int)lv_slider_get_value(slider);

    // This is USER preference. Policy cap will still apply to hardware.
    backlight_set_user_pct(s_user_pct);

    update_labels();
    mark_user_activity();
}

void open_brightness_modal(lv_obj_t *parent)
{
    if (brightness_modal) return;

    brightness_modal = lv_obj_create(parent);
    lv_obj_set_size(brightness_modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(brightness_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(brightness_modal, LV_OPA_70, 0);
    lv_obj_clear_flag(brightness_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(brightness_modal);
    lv_obj_set_size(card, 340, 220);
    lv_obj_center(card);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, "Brightness");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 8);

    // Slider represents USER preference, not effective.
    // So allow full range always; cap is shown separately and enforced by backlight module.
    s_slider = lv_slider_create(card);
    lv_obj_set_width(s_slider, lv_pct(90));
    lv_obj_align(s_slider, LV_ALIGN_CENTER, 0, 10);

    lv_slider_set_range(s_slider, 10, 100);

    // Start position = current USER preference
    s_user_pct = backlight_get_user_pct();
    if (s_user_pct < 10) s_user_pct = 10;
    if (s_user_pct > 100) s_user_pct = 100;

    lv_slider_set_value(s_slider, s_user_pct, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_slider, on_brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // Labels at bottom of card
    s_req_lbl = lv_label_create(card);
    lv_obj_align(s_req_lbl, LV_ALIGN_BOTTOM_LEFT, 12, -56);

    s_cap_lbl = lv_label_create(card);
    lv_obj_align(s_cap_lbl, LV_ALIGN_BOTTOM_LEFT, 12, -36);

    s_eff_lbl = lv_label_create(card);
    lv_obj_align(s_eff_lbl, LV_ALIGN_BOTTOM_LEFT, 12, -16);

    update_labels();

    lv_obj_t *save = lv_btn_create(card);
    lv_obj_set_size(save, 90, 36);
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, -12, -10);
    lv_obj_add_event_cb(save, brightness_modal_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save_lbl = lv_label_create(save);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_center(save_lbl);
}
