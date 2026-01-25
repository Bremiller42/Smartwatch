// FILE: src/ui/ui_screen_clock.c
#include "ui_priv.h"
#include "esp_log.h"
#include "watch_heartrate.h"
#include "ui_color_pallete.h"
#include "watch_fuel.h"
static const char *UI_SC_TAG = "UI_CLOCK";

static lv_obj_t *hr_btn = NULL;
static lv_obj_t *hr_btn_icon = NULL;
static lv_obj_t *hr_status_lbl = NULL;
static lv_obj_t *hr_bpm_lbl    = NULL;
static lv_obj_t *hr_prog_arc   = NULL;



static void on_back_to_home(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) ui_show(UI_HOME);
}

static void on_hr_read_now(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hr_request_read_now();
    mark_user_activity();
}
static void ui_hr_widget_refresh_async(void *arg)
{
    (void)arg;

    hr_ui_status_t st;
    hr_get_ui_status(&st);

    // status text
    switch (st.state) {
        case HR_STATE_IDLE:         lv_label_set_text(hr_status_lbl, "HR: Idle"); break;
        case HR_STATE_SEEK_CONTACT: lv_label_set_text(hr_status_lbl, "HR: No Contact"); break;
        case HR_STATE_STABILIZING:  lv_label_set_text(hr_status_lbl, "HR: Stabilizing..."); break;
        case HR_STATE_MEASURING:    lv_label_set_text(hr_status_lbl, "HR: Measuring..."); break;
        case HR_STATE_DONE:         lv_label_set_text(hr_status_lbl, "HR: Done"); break;
        case HR_STATE_ERROR:        lv_label_set_text(hr_status_lbl, "HR: Error"); break;
        default:                    lv_label_set_text(hr_status_lbl, "HR: --"); break;
    }

    // BPM (persisted "current" should show whenever valid)
    if (st.bpm_valid && st.bpm_current > 0.1f) {
        char b[16];
        snprintf(b, sizeof(b), "%.0f", st.bpm_current);
        lv_label_set_text(hr_bpm_lbl, b);
    } else {
        lv_label_set_text(hr_bpm_lbl, "--");
    }
    
    // Progress arc
    if (st.state == HR_STATE_MEASURING) {
        lv_obj_clear_flag(hr_prog_arc, LV_OBJ_FLAG_HIDDEN);
        int pct = 0;
        if (st.session_ms_total > 0) {
            pct = (int)((100ULL * st.session_ms_elapsed) / st.session_ms_total);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
        }
        lv_arc_set_value(hr_prog_arc, pct);
    } else {
        lv_obj_add_flag(hr_prog_arc, LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_value(hr_prog_arc, 0);
    }
}
void ui_hr_widget_refresh_request(void)
{
    lv_async_call(ui_hr_widget_refresh_async, NULL);
}
lv_obj_t *ui_build_clock_screen(void)
{   
    ESP_LOGI(UI_SC_TAG, "Building Clock UI");

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, activity_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_style_clip_corner(scr, false, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    clock_time_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(clock_time_lbl, &atomic_radio_58, 0);
    lv_obj_set_style_text_color(clock_time_lbl, UI_COLOR(THEME), 0);
    lv_obj_align(clock_time_lbl, LV_ALIGN_TOP_MID, 0, 30);

    clock_date_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(clock_date_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(clock_date_lbl, UI_COLOR(THEME), 0);
    lv_obj_align(clock_date_lbl, LV_ALIGN_TOP_LEFT, 5, 5);

    clock_wifi_icon = lv_label_create(scr);
    lv_obj_set_style_text_color(clock_wifi_icon, UI_COLOR(WHITE), 0);
    lv_obj_align(clock_wifi_icon, LV_ALIGN_TOP_RIGHT, -12, 6);
    lv_label_set_text(clock_wifi_icon, LV_SYMBOL_WIFI);

    clock_ble_icon = lv_label_create(scr);
    lv_obj_set_style_text_color(clock_ble_icon, UI_COLOR(BLUE), 0);
    lv_obj_align_to(clock_ble_icon, clock_wifi_icon, LV_ALIGN_OUT_LEFT_MID, 10, 0);
    lv_label_set_text(clock_ble_icon, LV_SYMBOL_BLUETOOTH);

    // NOTE: watch_batt_lbl exists in globals in your snippet
    watch_batt_lbl = lv_label_create(scr);
    lv_label_set_text(watch_batt_lbl, "--%%");
    lv_obj_set_style_text_color(watch_batt_lbl, UI_COLOR(battery_color(g_watch_batt_pct)), 0);
    lv_obj_set_style_text_font(watch_batt_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(watch_batt_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align_to(watch_batt_lbl, clock_ble_icon, LV_ALIGN_OUT_RIGHT_MID, -130, 0);

    phone_batt_lbl = lv_label_create(scr);
    lv_label_set_text(phone_batt_lbl, "");
    lv_obj_set_style_text_color(phone_batt_lbl, UI_COLOR(THEME), 0);
    lv_obj_set_style_text_font(phone_batt_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_width(phone_batt_lbl, 90);
    lv_obj_set_style_text_align(phone_batt_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align_to(phone_batt_lbl, scr, LV_ALIGN_LEFT_MID, -20, -55);

    // --- Heart rate status text ---
    hr_status_lbl = lv_label_create(scr);
    lv_label_set_text(hr_status_lbl, "HR: Idle");
    lv_obj_set_style_text_color(hr_status_lbl, UI_COLOR(RED), 0);
    lv_obj_set_style_text_font(hr_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_opa(hr_status_lbl, LV_OPA_80, 0);
    lv_obj_align(hr_status_lbl, LV_ALIGN_RIGHT_MID, -18, -30);

    // --- Big BPM ---
    hr_bpm_lbl = lv_label_create(scr);
    lv_label_set_text(hr_bpm_lbl, "--");
    lv_obj_set_style_text_color(hr_bpm_lbl, UI_COLOR(RED), 0);
    lv_obj_set_style_text_font(hr_bpm_lbl, &lv_font_montserrat_26, 0);   // or smaller if you want
    lv_obj_align(hr_bpm_lbl, LV_ALIGN_RIGHT_MID, -18, 10);

    // --- Progress arc (hidden unless measuring) ---
    hr_prog_arc = lv_arc_create(scr);
    lv_obj_set_size(hr_prog_arc, 52, 52);
    lv_arc_set_rotation(hr_prog_arc, 270);
    lv_arc_set_bg_angles(hr_prog_arc, 0, 360);
    lv_arc_set_range(hr_prog_arc, 0, 100);
    lv_arc_set_value(hr_prog_arc, 0);
    lv_obj_set_style_arc_width(hr_prog_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(hr_prog_arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(hr_prog_arc, UI_COLOR(RED), 1);
    lv_obj_align(hr_prog_arc, LV_ALIGN_RIGHT_MID, -10, -5);
    lv_obj_add_flag(hr_prog_arc, LV_OBJ_FLAG_HIDDEN);


    hr_btn = lv_btn_create(scr);
    lv_obj_set_size(hr_btn, 44, 44);
    lv_obj_set_style_radius(hr_btn, 10, 0);
    lv_obj_set_style_bg_color(hr_btn, UI_COLOR(BLACK), 0);
    lv_obj_set_style_bg_opa(hr_btn, LV_OPA_50, 0);
    lv_obj_set_style_border_width(hr_btn, 1, 0);
    lv_obj_set_style_border_color(hr_btn, lv_color_hex(0x404040), 0);
    lv_obj_clear_flag(hr_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hr_btn, on_hr_read_now, LV_EVENT_CLICKED, NULL);

    lv_obj_align_to(hr_btn, hr_prog_arc, LV_ALIGN_OUT_RIGHT_MID, -90, 15);

    hr_btn_icon = lv_img_create(hr_btn);
    lv_img_set_src(hr_btn_icon, &heart_pulse_solid_full_a8_32);
    lv_obj_center(hr_btn_icon);
    lv_obj_set_style_img_recolor(hr_btn_icon, UI_COLOR(RED), 0);
    lv_obj_set_style_img_recolor_opa(hr_btn_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_img_recolor(hr_btn_icon, lv_color_hex(0xFF5555), LV_STATE_PRESSED);


    lv_obj_t *menu = lv_btn_create(scr);
    lv_obj_set_size(menu, 90, 50);
    lv_obj_set_style_bg_color(menu, lv_color_black(), 0);
    lv_obj_set_style_text_color(menu, UI_COLOR(THEME), 0);

    lv_obj_align(menu, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    lv_obj_add_event_cb(menu, on_back_to_home, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(menu), LV_SYMBOL_HOME);
    lv_obj_center(lv_obj_get_child(menu, 0));
    
    ui_set_watch_batt(g_watch_batt_pct, g_watch_batt_v);
    clock_update_label_now();
    clock_update_wifi_icon_now();
    clock_update_ble_icon_now();
    ui_notif_bar_attach(scr);
    ui_hr_widget_refresh_request();
    ui_set_watch_batt(g_watch_batt_pct, g_watch_batt_v);
    ui_notif_refresh_async();

    if (clock_timer == NULL) {
        clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
    }

    return scr;
}
