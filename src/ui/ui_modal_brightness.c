// FILE: src/ui/ui_modal_brightness.c
#include "ui_priv.h"
#include "esp_log.h"

static const char *UI_BRT_TAG = "UI_BRIGHT";


static lv_obj_t *brightness_modal = NULL;

static void brightness_modal_close(lv_event_t *e)
{
    (void)e;
    if (brightness_modal) {
        lv_obj_del(brightness_modal);
        brightness_modal = NULL;
    }
}

static void on_brightness_changed(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *slider = lv_event_get_target(e);
    g_brightness = lv_slider_get_value(slider);

    apply_backlight_percent(g_brightness);

    ESP_LOGI(UI_BRT_TAG, "Brightness=%d -> saving", g_brightness);
    settings_save_brightness(g_brightness);

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

    brightness_slider = lv_slider_create(card);
    lv_obj_set_width(brightness_slider, lv_pct(90));
    lv_obj_align(brightness_slider, LV_ALIGN_CENTER, 0, 15);
    lv_slider_set_range(brightness_slider, 10, 100);
    lv_slider_set_value(brightness_slider, g_brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(brightness_slider, on_brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *close = lv_btn_create(card);
    lv_obj_set_size(close, 90, 36);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_event_cb(close, brightness_modal_close, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(close), "Close");
    lv_obj_center(lv_obj_get_child(close, 0));
}
