// FILE: src/ui/ui_screen_home.c
#include "ui_priv.h"
#include "esp_log.h"
static const char *UI_SH_TAG = "UI_HOME";

static const int16_t tile_w = 100;
static const int16_t tile_h = 100;

static void on_open_clock(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) ui_show(UI_CLOCK);
}
static void on_open_settings(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) ui_show(UI_SETTINGS);
}
static void on_log_open(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_show(UI_LOG);
}

lv_obj_t *ui_build_home_screen(void)
{
    ESP_LOGI(UI_SH_TAG, "Building Home Screen");

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_add_event_cb(scr, activity_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *btn_clock = lv_btn_create(scr);
    lv_obj_set_size(btn_clock, tile_w, tile_h);
    lv_obj_align(btn_clock, LV_ALIGN_CENTER, -90, -70);
    lv_obj_add_event_cb(btn_clock, on_open_clock, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(btn_clock), "Clock");
    lv_obj_center(lv_obj_get_child(btn_clock, 0));

    lv_obj_t *btn_settings = lv_btn_create(scr);
    lv_obj_set_size(btn_settings, tile_w, tile_h);
    lv_obj_align(btn_settings, LV_ALIGN_CENTER, -90, 70);
    lv_obj_add_event_cb(btn_settings, on_open_settings, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(btn_settings), "Settings");
    lv_obj_center(lv_obj_get_child(btn_settings, 0));

    lv_obj_t *btn_log = lv_btn_create(scr);
    lv_obj_set_size(btn_log, tile_w, tile_h);
    lv_obj_align(btn_log, LV_ALIGN_CENTER, 90, -70);
    lv_obj_add_event_cb(btn_log, on_log_open, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(btn_log), "Log");
    lv_obj_center(lv_obj_get_child(btn_log, 0));

    return scr;
}
