// FILE: src/ui/ui_screen_home.c  (or rename to ui_screen_power.c later)
#include "ui_priv.h"
#include "esp_log.h"
#include "esp_system.h"   // <-- for esp_restart()
#include "watch_power.h"

static const char *UI_SH_TAG = "UI_POWER_MENU";

static const int16_t tile_w = 75;
static const int16_t tile_h = 75;

static lv_obj_t *restart_icon = NULL;
static lv_obj_t *shutdown_icon = NULL;

static void on_shutdown(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ui_show(UI_BLANK);
        watch_power_shutdown_async();   // <-- FIXED ;
    }
}

static void on_restart(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ui_show(UI_BLANK);
        watch_power_restart_async();
    }
}

static void on_back(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_show(UI_HOME);
}

lv_obj_t *ui_build_power_menu(void)
{
    ESP_LOGI(UI_SH_TAG, "Building Power Menu");

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_add_event_cb(scr, activity_event_cb, LV_EVENT_ALL, NULL);

    // Restart button
    lv_obj_t *btn_restart = lv_btn_create(scr);
    lv_obj_set_size(btn_restart, tile_w, tile_h);
    lv_obj_align(btn_restart, LV_ALIGN_CENTER, 60, 0);
    lv_obj_add_event_cb(btn_restart, on_restart, LV_EVENT_CLICKED, NULL);

    restart_icon = lv_img_create(btn_restart);
    lv_img_set_src(restart_icon, &rotate_left_solid_full_a8_50);
    lv_obj_center(restart_icon);
    lv_obj_set_style_img_recolor(restart_icon, lv_color_black(), 0);
    lv_obj_set_style_img_recolor_opa(restart_icon, LV_OPA_COVER, 0);
    
    // Shutdown button
    lv_obj_t *btn_shutdown = lv_btn_create(scr);
    lv_obj_set_size(btn_shutdown, tile_w, tile_h);
    lv_obj_align(btn_shutdown, LV_ALIGN_CENTER, -60, 0);
    lv_obj_add_event_cb(btn_shutdown, on_shutdown, LV_EVENT_CLICKED, NULL);

    shutdown_icon = lv_img_create(btn_shutdown);
    lv_img_set_src(shutdown_icon, &power_off_solid_full_a8_50);
    lv_obj_center(shutdown_icon);
    lv_obj_set_style_img_recolor(shutdown_icon, lv_color_black(), 0);
    lv_obj_set_style_img_recolor_opa(shutdown_icon, LV_OPA_COVER, 0);
    
    lv_obj_t *back = lv_btn_create(scr);
    lv_obj_set_size(back, 25, 25);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_60, 0);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btxt = lv_label_create(back);
    lv_label_set_text(btxt, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(btxt, lv_color_white(), 0);
    lv_obj_center(btxt);


    return scr;
}
