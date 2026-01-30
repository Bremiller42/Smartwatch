// FILE: src/ui/ui_screen_clock.c
#include "ui_priv.h"
#include "esp_log.h"
#include "watch_heartrate.h"
#include "ui_color_pallete.h"
#include "watch_fuel.h"
#include "watch_weather.h"
#include "watch_icons/watch_icons.h"

static const char *UI_SC_TAG = "UI_CLOCK";

static lv_obj_t *hr_btn = NULL;
static lv_obj_t *hr_btn_icon = NULL;
static lv_obj_t *hr_status_lbl = NULL;
static lv_obj_t *hr_bpm_lbl    = NULL;
static lv_obj_t *hr_prog_arc   = NULL;
static lv_obj_t *wx_temp_lbl = NULL;
static lv_obj_t *wx_hum_lbl  = NULL;
static lv_obj_t *wx_hum_icon = NULL;
static lv_obj_t *wx_icon_main = NULL;  // lv_img
static lv_obj_t *wx_icon_wind = NULL;  // lv_img

static bool s_wx_cb_registered = false;

static void on_back_to_home(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) ui_show(UI_HOME);
}
static const void *wx_icon_for_ow_icon(const char *icon_code)
{
    // OpenWeather icon codes:
    // 01 clear, 02 few clouds, 03 scattered, 04 broken
    // 09 shower rain, 10 rain, 11 thunder, 13 snow, 50 mist
    if (!icon_code || icon_code[0] == '\0') return &icon_cloud_regular_42;

    bool night = (strlen(icon_code) >= 3 && icon_code[2] == 'n');

    // Match by the first two chars
    if (icon_code[0] == '0' && icon_code[1] == '1') {
        return night ? (const void*)&icon_moon_solid_42 : (const void*)&icon_sun_solid_42;
    }
    if (icon_code[0] == '0' && icon_code[1] == '2') {
        // You don't currently have a dedicated partly-cloudy 42 icon;
        // pick cloud vs sun/moon based on day/night.
        return night ? (const void*)&icon_cloud_solid_42 : (const void*)&icon_cloud_solid_42;
    }
    if (icon_code[0] == '0' && (icon_code[1] == '3' || icon_code[1] == '4')) {
        return &icon_cloud_solid_42;
    }
    if (icon_code[0] == '0' && icon_code[1] == '9') {
        return &icon_cloud_rain_solid_42;
    }
    if (icon_code[0] == '1' && icon_code[1] == '0') {
        return &icon_cloud_rain_solid_42;
    }
    if (icon_code[0] == '1' && icon_code[1] == '1') {
        return &icon_cloud_bolt_solid_42;
    }
    if (icon_code[0] == '1' && icon_code[1] == '3') {
        return &icon_snowflake_solid_42;
    }
    if (icon_code[0] == '5' && icon_code[1] == '0') {
        // mist/fog: you chose "wind-ish"
        return &icon_wind_solid_42;
    }

    return &icon_cloud_regular_42;
}


static bool wx_is_windy_mph10(int wind_mps_x10)
{
    // 10 mph = 4.4704 m/s -> 44.704 in (m/s * 10)
    const int threshold_mps_x10 = 45;
    return wind_mps_x10 >= threshold_mps_x10;
}
static void wx_refresh_async(void *arg)
{
    (void)arg;

    weather_snapshot_t s;
    if (!weather_get_latest(&s) || !s.valid) {
        if (wx_temp_lbl) lv_label_set_text(wx_temp_lbl, "----");
        if (wx_hum_lbl)  lv_label_set_text(wx_hum_lbl,  "----");
        if (wx_icon_main) lv_img_set_src(wx_icon_main, &icon_cloud_regular_42);
        if (wx_icon_wind) lv_obj_add_flag(wx_icon_wind, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // Temp: show in F if configured
    int temp_x10 = s.temp_c_x10;
    const char *unit = "°C";
    if (weather_get_use_fahrenheit()) {
        temp_x10 = (s.temp_c_x10 * 9) / 5 + 320;
        unit = "°F";
    }
    char tbuf[24];

    snprintf(tbuf, sizeof(tbuf), "%d.%d%s", temp_x10/10, abs(temp_x10%10), unit);

    char hbuf[24];
    snprintf(hbuf, sizeof(hbuf), "%d%%", s.humidity_pct);

    if (wx_temp_lbl) lv_label_set_text(wx_temp_lbl, tbuf);
    if (wx_hum_lbl)  lv_label_set_text(wx_hum_lbl,  hbuf);

    // Main icon
    if (wx_icon_main) {
        const void *src = NULL;

        if (s.icon_code[0] != '\0') {
            src = wx_icon_for_ow_icon(s.icon_code);
        } else {
            // Fallback: derive from is_day
            src = s.is_day ? (const void*)&icon_sun_solid_42 : (const void*)&icon_moon_solid_42;
        }

        lv_img_set_src(wx_icon_main, src);
    }

    // Wind icon toggle
    if (wx_icon_wind) {
        if (wx_is_windy_mph10(s.wind_mps_x10)) {
            lv_obj_clear_flag(wx_icon_wind, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(wx_icon_wind, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void wx_on_weather_update_cb(const weather_snapshot_t *snap)
{
    (void)snap;
    lv_async_call(wx_refresh_async, NULL);
}

static void on_hr_read_now(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    if (!hr_allowed_by_power()) {
        // optional small haptic/flash message later
        ui_hr_widget_refresh_request();
        return;
    }

    hr_request_read_now();
    mark_user_activity();
}


static void ui_hr_set_enabled(bool en)
{
    if (!hr_btn || !hr_btn_icon || !hr_status_lbl || !hr_bpm_lbl || !hr_prog_arc) return;

    if (en) {
        // enable interaction
        lv_obj_clear_state(hr_btn, LV_STATE_DISABLED);
        lv_obj_add_flag(hr_btn, LV_OBJ_FLAG_CLICKABLE);

        // colors back
        lv_obj_set_style_bg_opa(hr_btn, LV_OPA_50, 0);
        lv_obj_set_style_img_recolor_opa(hr_btn_icon, LV_OPA_COVER, 0);
        lv_obj_set_style_text_opa(hr_status_lbl, LV_OPA_80, 0);
        lv_obj_set_style_text_opa(hr_bpm_lbl, LV_OPA_COVER, 0);

        // restore theme-ish colors
        lv_obj_set_style_text_color(hr_status_lbl, UI_COLOR(RED), 0);
        lv_obj_set_style_text_color(hr_bpm_lbl, UI_COLOR(RED), 0);
        lv_obj_set_style_img_recolor(hr_btn_icon, UI_COLOR(RED), 0);
    } else {
        // disable interaction
        lv_obj_add_state(hr_btn, LV_STATE_DISABLED);

        // visually grey
        lv_obj_set_style_bg_opa(hr_btn, LV_OPA_20, 0);
        lv_obj_set_style_img_recolor(hr_btn_icon, lv_color_hex(0x777777), 0);
        lv_obj_set_style_img_recolor_opa(hr_btn_icon, LV_OPA_COVER, 0);

        lv_obj_set_style_text_color(hr_status_lbl, lv_color_hex(0x777777), 0);
        lv_obj_set_style_text_color(hr_bpm_lbl,    lv_color_hex(0x777777), 0);

        // optional: force text to "Disabled"
        lv_label_set_text(hr_status_lbl, "HR: Power Save");
        lv_label_set_text(hr_bpm_lbl, "---");

        // hide progress
        lv_obj_add_flag(hr_prog_arc, LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_value(hr_prog_arc, 0);
    }
}



static void ui_hr_widget_refresh_async(void *arg)
{
    (void)arg;
    if (!hr_status_lbl || !hr_bpm_lbl || !hr_prog_arc) return;
    bool ok = hr_allowed_by_power();
    ui_hr_set_enabled(ok);
    if (!ok) return;   // stop here so normal HR states don't overwrite "Power Save"
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
        lv_label_set_text(hr_bpm_lbl, "---");
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

    // =============================
    // Heart Rate cluster (anchor: button @ mid-right)
    // =============================
    hr_btn = lv_btn_create(scr);
    lv_obj_set_size(hr_btn, 44, 44);
    lv_obj_set_style_radius(hr_btn, 10, 0);
    lv_obj_set_style_bg_color(hr_btn, UI_COLOR(BLACK), 0);
    lv_obj_set_style_bg_opa(hr_btn, LV_OPA_50, 0);
    lv_obj_set_style_border_width(hr_btn, 1, 0);
    lv_obj_clear_flag(hr_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hr_btn, on_hr_read_now, LV_EVENT_CLICKED, NULL);

    // Anchor HR button to middle-right
    lv_obj_align(hr_btn, LV_ALIGN_RIGHT_MID, -12, 10);

    hr_btn_icon = lv_img_create(hr_btn);
    lv_img_set_src(hr_btn_icon, &heart_pulse_solid_full_a8_32);
    lv_obj_center(hr_btn_icon);
    lv_obj_set_style_img_recolor_opa(hr_btn_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_img_recolor(hr_btn_icon, lv_color_hex(0xFF5555), LV_STATE_PRESSED);

    // --- Big BPM (left of button) ---
    hr_bpm_lbl = lv_label_create(scr);
    // Fixed width box + right-justified text
    lv_obj_set_width(hr_bpm_lbl, 80);
    lv_obj_set_style_text_align(hr_bpm_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(hr_bpm_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_align_to(hr_bpm_lbl, hr_btn, LV_ALIGN_OUT_LEFT_MID, -10, -10);

    lv_label_set_text(hr_bpm_lbl, "---");
    lv_obj_set_style_text_font(hr_bpm_lbl, &lv_font_montserrat_26, 0);

    // --- Progress arc (centered over BPM) ---
    hr_prog_arc = lv_arc_create(scr);
    lv_obj_set_size(hr_prog_arc, 50, 50);
    lv_arc_set_rotation(hr_prog_arc, 270);
    lv_arc_set_bg_angles(hr_prog_arc, 0, 360);
    lv_arc_set_range(hr_prog_arc, 0, 100);
    lv_arc_set_value(hr_prog_arc, 0);
    lv_obj_set_style_arc_width(hr_prog_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(hr_prog_arc, 6, LV_PART_INDICATOR);

    // Make arc fill RED, hide knob/"bubble"
    lv_obj_set_style_arc_color(hr_prog_arc, UI_COLOR(THEME), LV_PART_MAIN);

    lv_obj_set_style_arc_color(hr_prog_arc, UI_COLOR(RED), LV_PART_INDICATOR);
    lv_obj_set_style_opa(hr_prog_arc, LV_OPA_TRANSP, LV_PART_KNOB);

    lv_obj_align_to(hr_prog_arc, hr_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(hr_prog_arc, LV_OBJ_FLAG_HIDDEN);

    // --- Status (above both; right edge aligned to button right) ---
    hr_status_lbl = lv_label_create(scr);
    lv_label_set_text(hr_status_lbl, "HR: Idle");
    lv_obj_set_style_text_font(hr_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_opa(hr_status_lbl, LV_OPA_80, 0);

    // Fixed width box + right-justified text
    lv_obj_set_width(hr_status_lbl, 160);
    lv_obj_set_style_text_align(hr_status_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(hr_status_lbl, LV_LABEL_LONG_CLIP);

    // Anchor status ABOVE the button, but keep it spanning over bpm+btn
    // (right edge locks to button's right edge)
    lv_obj_align_to(hr_status_lbl, hr_btn, LV_ALIGN_OUT_TOP_RIGHT, 0, -5);

    lv_obj_t *menu = lv_btn_create(scr);
    lv_obj_set_size(menu, 90, 50);
    lv_obj_set_style_bg_color(menu, lv_color_black(), 0);
    lv_obj_set_style_text_color(menu, UI_COLOR(THEME), 0);

    lv_obj_align(menu, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    lv_obj_add_event_cb(menu, on_back_to_home, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(menu), LV_SYMBOL_HOME);
    lv_obj_center(lv_obj_get_child(menu, 0));
    // =============================
    // Weather cluster (anchor: bottom-right)
    // bottom->top: humidity, temp, icons row (wind + main)
    // =============================

    // Humidity (bottom-right)
    wx_hum_lbl = lv_label_create(scr);
    lv_obj_set_width(wx_hum_lbl, 40);
    lv_obj_set_style_text_align(wx_hum_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(wx_hum_lbl, LV_LABEL_LONG_CLIP);

    lv_obj_set_style_text_font(wx_hum_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(wx_hum_lbl, UI_COLOR(THEME), 0);
    lv_obj_set_style_text_opa(wx_hum_lbl, LV_OPA_COVER, 0);
    lv_label_set_text(wx_hum_lbl, "----");
    lv_obj_align(wx_hum_lbl, LV_ALIGN_BOTTOM_RIGHT, -12, -18);

    wx_hum_icon = lv_img_create(scr);
    lv_img_set_src(wx_hum_icon, &droplet_solid_full_a8_20);
    lv_obj_set_style_img_recolor_opa(wx_hum_icon, LV_OPA_90, 0);
    lv_obj_set_style_img_recolor(wx_hum_icon, UI_COLOR(THEME), 0);
    lv_obj_align_to(wx_hum_icon, wx_hum_lbl, LV_ALIGN_OUT_LEFT_MID, -7, 0);
    lv_obj_set_style_bg_opa(wx_hum_icon, LV_OPA_0, 0);
    lv_obj_move_background(wx_hum_icon);
    lv_obj_move_foreground(wx_hum_lbl);

    // Temp (above humidity)
    wx_temp_lbl = lv_label_create(scr);
    lv_obj_set_width(wx_temp_lbl, 60);
    lv_obj_set_style_text_align(wx_temp_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(wx_temp_lbl, LV_LABEL_LONG_CLIP);

    lv_obj_set_style_text_font(wx_temp_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(wx_temp_lbl, UI_COLOR(WHITE), 0);
    lv_label_set_text(wx_temp_lbl, "--°F");
    lv_obj_align_to(wx_temp_lbl, wx_hum_lbl, LV_ALIGN_OUT_TOP_RIGHT, -5, -4);

    // Icons row (above temp): wind + main side-by-side
    wx_icon_main = lv_img_create(scr);
    lv_img_set_src(wx_icon_main, &icon_cloud_regular_42);
    lv_obj_set_style_img_recolor(wx_icon_main, UI_COLOR(WHITE), 0);
    lv_obj_set_style_img_recolor_opa(wx_icon_main, LV_OPA_COVER, 0);

    // Anchor main icon above temp, right aligned
    lv_obj_align_to(wx_icon_main, wx_temp_lbl, LV_ALIGN_OUT_TOP_RIGHT, 0, -6);

    wx_icon_wind = lv_img_create(scr);
    lv_img_set_src(wx_icon_wind, &icon_wind_solid_42);
    lv_obj_set_style_img_recolor(wx_icon_wind, UI_COLOR(THEME), 0);
    lv_obj_set_style_img_recolor_opa(wx_icon_wind, LV_OPA_COVER, 0);

    // Wind sits to the LEFT of main icon
    lv_obj_align_to(wx_icon_wind, wx_icon_main, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    lv_obj_add_flag(wx_icon_wind, LV_OBJ_FLAG_HIDDEN);


    ui_set_watch_batt(g_watch_batt_pct, g_watch_batt_v);
    clock_update_label_now();
    clock_update_wifi_icon_now();
    clock_update_ble_icon_now();

    // Register weather update callback once
    if (!s_wx_cb_registered) {
        weather_register_cb(wx_on_weather_update_cb);
        s_wx_cb_registered = true;
    }

    // Show whatever we currently have (cached)
    lv_async_call(wx_refresh_async, NULL);

    // Also request a fresh update now that the clock screen exists
    // (it will be throttled by your min_refresh anyway)
    weather_request_update();

    ui_notif_bar_attach(scr);
    ui_set_watch_batt(g_watch_batt_pct, g_watch_batt_v);
    // Force initial HR widget state immediately (not async)
    ui_hr_set_enabled(hr_allowed_by_power());

    // Also apply the correct text/progress immediately if you want:
    ui_hr_widget_refresh_request();

    ui_notif_refresh_async();

    if (clock_timer == NULL) {
        clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
    }

    return scr;
}
