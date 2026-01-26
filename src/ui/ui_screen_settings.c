// FILE: src/ui/ui_screen_settings.c
#include "ui_priv.h"
#include "esp_log.h"
#include "watch_screen_timeout.h"

static const char *UI_SS_TAG = "UI_SETTINGS";

/* local (settings-only) labels */
static lv_obj_t *timeout_sub_lbl = NULL;
static lv_obj_t *hr_period_sub_lbl = NULL;

/* ---- Timeout tile helpers ---- */
static uint32_t timeout_get_s(void)
{
    uint32_t s = screen_timeout_get_default_ms() / 1000;
    if (s != 15 && s != 30 && s != 60) s = 15;
    return s;
}

static void timeout_update_subtitle(void)
{
    if (!timeout_sub_lbl) return;

    uint32_t s = timeout_get_s();
    static char buf[16];
    snprintf(buf, sizeof(buf), "%lus", (unsigned long)s);
    lv_label_set_text(timeout_sub_lbl, buf);
}

static void on_timeout_tile_clicked(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    uint32_t s = timeout_get_s();
    if (s == 15) s = 30;
    else if (s == 30) s = 60;
    else s = 15;

    screen_timeout_set_default_ms(s * 1000);
    settings_save_screen_timeout_s(s);

    timeout_update_subtitle();
    mark_user_activity();
}

/* ---- HR period preset ---- */
typedef enum {
    HRP_OFF = 0,
    HRP_ALWAYS_ON,
    HRP_30S,
    HRP_1M,
    HRP_5M,
    HRP_10M,
    HRP_1H,
    HRP_MAX
} hr_period_preset_t;

static hr_period_preset_t s_hr_preset = HRP_OFF;

static void hr_period_apply_preset(hr_period_preset_t p)
{
    s_hr_preset = p;

    switch (p) {
        case HRP_OFF:       g_hr_period_ms = 0;           g_hr_run_ms = 0;        break;
        case HRP_ALWAYS_ON: g_hr_period_ms = 1000;        g_hr_run_ms = 1000;     break;
        case HRP_30S:       g_hr_period_ms = 30 * 1000;   g_hr_run_ms = 10 * 1000;break;
        case HRP_1M:        g_hr_period_ms = 60 * 1000;   g_hr_run_ms = 10 * 1000;break;
        case HRP_5M:        g_hr_period_ms = 5 * 60 * 1000; g_hr_run_ms = 10 * 1000;break;
        case HRP_10M:       g_hr_period_ms = 10 * 60 * 1000; g_hr_run_ms = 10 * 1000;break;
        case HRP_1H:        g_hr_period_ms = 60 * 60 * 1000; g_hr_run_ms = 10 * 1000;break;
        default: break;
    }
}

static const char *hr_period_preset_text(hr_period_preset_t p)
{
    switch (p) {
        case HRP_OFF:       return "Off";
        case HRP_ALWAYS_ON: return "Always On";
        case HRP_30S:       return "30s";
        case HRP_1M:        return "1m";
        case HRP_5M:        return "5m";
        case HRP_10M:       return "10m";
        case HRP_1H:        return "1h";
        default:            return "";
    }
}

static void hr_period_update_subtitle(void)
{
    if (!hr_period_sub_lbl) return;
    lv_label_set_text(hr_period_sub_lbl, hr_period_preset_text(s_hr_preset));
}

static void on_hr_period_tile_clicked(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    hr_period_preset_t next = (hr_period_preset_t)((s_hr_preset + 1) % HRP_MAX);
    hr_period_apply_preset(next);
    hr_period_update_subtitle();

    if (s_hr_preset != HRP_OFF) {
        hr_request_read_now();
    }

    mark_user_activity();
}

/* ---- callbacks ---- */
static void on_settings_back(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) ui_show(UI_HOME);
}

static void on_open_brightness(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_brightness_modal(lv_scr_act());
}

static void on_open_wifi_picker(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_wifi_picker_modal(lv_scr_act());
}

static void on_open_settime(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) open_settime_modal(lv_scr_act());
}

static void on_time_format_changed(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *sw = lv_event_get_target(e);
    use_24h_format = lv_obj_has_state(sw, LV_STATE_CHECKED);

    ESP_LOGI(UI_SS_TAG, "use24h toggled -> %d", (int)use_24h_format);
    settings_save_24h(use_24h_format);

    mark_user_activity();
}

static void on_wifi_toggle(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    g_wifi_on = on;
    settings_save_wifi(on);

    if (on) {
        if (g_wifi_ssid[0]) {
            ESP_LOGI(UI_SS_TAG, "WiFi toggle ON -> connecting to saved SSID");
            wifi_start_sta(g_wifi_ssid, g_wifi_pass);
        } else {
            ESP_LOGI(UI_SS_TAG, "WiFi toggle ON -> no saved SSID, open picker");
            open_wifi_picker_modal(lv_scr_act());
        }
    } else {
        ESP_LOGI(UI_SS_TAG, "WiFi toggle OFF");
        wifi_stop();
    }

    mark_user_activity();
}

static void on_ble_toggle(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    g_ble_on = on;
    settings_save_ble(on);

    ble_set_enabled(on);

    // UI refresh
    lv_async_call(ui_update_ble_icon_async, NULL);
    lv_async_call(ui_update_ble_status_async, NULL);

    mark_user_activity();
}

lv_obj_t *ui_build_settings_screen(void)
{
    ESP_LOGI(UI_SS_TAG, "Building Settings Screen");

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, activity_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *tv = lv_tabview_create(scr, LV_DIR_TOP, 48);
    lv_obj_set_size(tv, lv_pct(100), lv_pct(100));
    lv_obj_align(tv, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(tv, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, 0);

    lv_obj_t *content = lv_tabview_get_content(tv);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(content, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(content, 0, 0);

    lv_obj_t *tab_bar = lv_tabview_get_tab_btns(tv);
    lv_obj_set_style_bg_color(tab_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tab_bar, 0, 0);
    lv_obj_set_style_pad_all(tab_bar, 6, 0);

    lv_obj_set_style_text_color(tab_bar, lv_color_hex(0xAAAAAA), LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(tab_bar, lv_color_white(),      LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_0,  LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_20, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_radius(tab_bar, 10, LV_PART_ITEMS);

    lv_obj_t *tab_display = lv_tabview_add_tab(tv, "Display");
    lv_obj_t *tab_network = lv_tabview_add_tab(tv, "Network");
    lv_obj_t *tab_time    = lv_tabview_add_tab(tv, "Time/Date");

    static lv_coord_t col_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static lv_coord_t row_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };

    // DISPLAY GRID
    lv_obj_t *g_disp = lv_obj_create(tab_display);
    lv_obj_set_size(g_disp, lv_pct(100), lv_pct(100));
    lv_obj_center(g_disp);
    lv_obj_set_style_bg_opa(g_disp, LV_OPA_0, 0);
    lv_obj_set_style_border_width(g_disp, 0, 0);
    lv_obj_set_style_pad_all(g_disp, 14, 0);
    lv_obj_set_style_pad_row(g_disp, 14, 0);
    lv_obj_set_style_pad_column(g_disp, 14, 0);
    lv_obj_clear_flag(g_disp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_grid_dsc_array(g_disp, col_dsc, row_dsc);

    lv_obj_t *t_bright = tile_create_nav_tile(g_disp, "Brightness", "Adjust", on_open_brightness);
    lv_obj_set_grid_cell(t_bright, LV_GRID_ALIGN_CENTER, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);

    lv_obj_t *t_timeout = tile_create_base(g_disp, "Timeout", "", &timeout_sub_lbl);
    tile_set_on(t_timeout, true);
    lv_obj_set_grid_cell(t_timeout, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    timeout_update_subtitle();
    lv_obj_add_event_cb(t_timeout, on_timeout_tile_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t_placeholder1 = tile_create_base(g_disp, "Theme", "Later", NULL);
    tile_set_on(t_placeholder1, false);
    lv_obj_set_grid_cell(t_placeholder1, LV_GRID_ALIGN_CENTER, 0, 1, LV_GRID_ALIGN_CENTER, 1, 1);

    lv_obj_t *t_placeholder2 = tile_create_base(g_disp, "Always On", "Later", NULL);
    tile_set_on(t_placeholder2, false);
    lv_obj_set_grid_cell(t_placeholder2, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 1, 1);

    // NETWORK GRID
    lv_obj_t *g_net = lv_obj_create(tab_network);
    lv_obj_set_size(g_net, lv_pct(100), lv_pct(100));
    lv_obj_center(g_net);
    lv_obj_set_style_bg_opa(g_net, LV_OPA_0, 0);
    lv_obj_set_style_border_width(g_net, 0, 0);
    lv_obj_set_style_pad_all(g_net, 14, 0);
    lv_obj_set_style_pad_row(g_net, 14, 0);
    lv_obj_set_style_pad_column(g_net, 14, 0);
    lv_obj_clear_flag(g_net, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_grid_dsc_array(g_net, col_dsc, row_dsc);

    lv_obj_t *wifi_sub = NULL;
    lv_obj_t *t_wifi = tile_create_switch_tile(g_net, "WiFi", g_ip_str, g_wifi_on, on_wifi_toggle, &wifi_sub);
    lv_obj_set_grid_cell(t_wifi, LV_GRID_ALIGN_CENTER, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    ip_lbl = wifi_sub;

    lv_obj_t *t_networks = tile_create_nav_tile(
        g_net, "Networks",
        (g_wifi_ssid[0] != '\0') ? g_wifi_ssid : "Choose WiFi",
        on_open_wifi_picker
    );
    lv_obj_set_grid_cell(t_networks, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);

    lv_obj_t *ble_sub = NULL;
    lv_obj_t *t_ble = tile_create_switch_tile(g_net, "BLE", "Off", g_ble_on, on_ble_toggle, &ble_sub);
    lv_obj_set_grid_cell(t_ble, LV_GRID_ALIGN_CENTER, 0, 1, LV_GRID_ALIGN_CENTER, 1, 1);
    ble_status_lbl = ble_sub;
    lv_async_call(ui_update_ble_status_async, NULL);

    lv_obj_t *t_notif = tile_create_nav_tile(g_net, "Notifications", "Coming soon", NULL);
    lv_obj_set_grid_cell(t_notif, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 1, 1);

    // TIME/DATE GRID
    lv_obj_t *g_time = lv_obj_create(tab_time);
    lv_obj_set_size(g_time, lv_pct(100), lv_pct(100));
    lv_obj_center(g_time);
    lv_obj_set_style_bg_opa(g_time, LV_OPA_0, 0);
    lv_obj_set_style_border_width(g_time, 0, 0);
    lv_obj_set_style_pad_all(g_time, 14, 0);
    lv_obj_set_style_pad_row(g_time, 14, 0);
    lv_obj_set_style_pad_column(g_time, 14, 0);
    lv_obj_clear_flag(g_time, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_grid_dsc_array(g_time, col_dsc, row_dsc);

    lv_obj_t *t_24h = tile_create_switch_tile(g_time, "24h Time", "", use_24h_format, on_time_format_changed, NULL);
    lv_obj_set_grid_cell(t_24h, LV_GRID_ALIGN_CENTER, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);

    lv_obj_t *t_set = tile_create_nav_tile(g_time, "Set Time", "Manual", on_open_settime);
    lv_obj_set_grid_cell(t_set, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);

    lv_obj_t *t_hr = tile_create_base(g_time, "Heart Rate", "", &hr_period_sub_lbl);
    tile_set_on(t_hr, true);
    lv_obj_set_grid_cell(t_hr, LV_GRID_ALIGN_CENTER, 2, 1, LV_GRID_ALIGN_CENTER, 1, 1);

    hr_period_apply_preset(s_hr_preset);
    hr_period_update_subtitle();
    lv_obj_add_event_cb(t_hr, on_hr_period_tile_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t_date = tile_create_base(g_time, "Date", "Later", NULL);
    tile_set_on(t_date, false);
    lv_obj_set_grid_cell(t_date, LV_GRID_ALIGN_CENTER, 0, 1, LV_GRID_ALIGN_CENTER, 1, 1);

    lv_obj_t *t_sync = tile_create_base(g_time, "Sync", "SNTP", NULL);
    tile_set_on(t_sync, true);
    lv_obj_set_grid_cell(t_sync, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 1, 1);

    // Top-left back button overlay
    lv_obj_t *back = lv_btn_create(scr);
    lv_obj_set_size(back, 20, 20);
    lv_obj_set_style_radius(back, 4, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(back, 0, 0);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_add_event_cb(back, on_settings_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btxt = lv_label_create(back);
    lv_label_set_text(btxt, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(btxt, lv_color_white(), 0);
    lv_obj_center(btxt);

    return scr;
}
