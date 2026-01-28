// FILE: src/ui/ui_modal_brightness.c
#include "ui_priv.h"
#include "esp_log.h"

#include "watch_backlight.h"
#include "watch_settings.h"

static const char *UI_BRT_TAG = "UI_BRIGHT";

static lv_obj_t *brightness_modal = NULL;
static lv_obj_t *s_slider = NULL;
static int user_pct = 0;
static void brightness_modal_close(lv_event_t *e)
{
    (void)e;
    if (brightness_modal) {
        ESP_LOGI(UI_BRT_TAG, "Brightness user=%d -> saving", user_pct);
        settings_save_brightness(user_pct);
        
        lv_obj_del(brightness_modal);
        brightness_modal = NULL;
        s_slider = NULL;
    }
}

static void on_brightness_changed(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *slider = lv_event_get_target(e);
    user_pct = (int)lv_slider_get_value(slider);

    // Set user preference (does not exceed cap in UI range, but this is still “user”)
    backlight_set_user_pct(user_pct);

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

    // Cap is policy (battery/thermal). User can only pick up to cap here.
    int cap = backlight_get_cap_pct();
    if (cap < 10) cap = 10;

    s_slider = lv_slider_create(card);
    lv_obj_set_width(s_slider, lv_pct(90));
    lv_obj_align(s_slider, LV_ALIGN_CENTER, 0, 15);

    lv_slider_set_range(s_slider, 10, cap);

    // What the screen actually shows right now
    int eff = backlight_get_effective_pct();
    if (eff < 10) eff = 10;
    if (eff > cap) eff = cap;

    lv_slider_set_value(s_slider, eff, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_slider, on_brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *save = lv_btn_create(card);
    lv_obj_set_size(save, 90, 36);
    lv_obj_align(save, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_event_cb(save, brightness_modal_close, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(save), "Save");
    lv_obj_center(lv_obj_get_child(save, 0));
}
