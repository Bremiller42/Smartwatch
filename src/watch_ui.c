// FILE: src/ui/watch_ui.c
#include "ui_priv.h"
#include "esp_log.h"
#include "watch_globals.h"
#include "watch_fuel.h"
const char *UI_TAG = "UI";

void ui_show(ui_screen_t s)
{
    ESP_LOGI(UI_TAG, "UI switch to %d", s);
    if (s == g_ui_current) return;

    if (g_ui_current == UI_LOG) {
        ui_log_screen_pause(true);
    }

    if (s == UI_HOME) {
        if (!scr_home) scr_home = ui_build_home_screen();
        lv_scr_load(scr_home);

    } else if (s == UI_CLOCK) {
        if (!scr_clock) scr_clock = ui_build_clock_screen();
        lv_scr_load(scr_clock);

        clock_update_label_now();
        clock_update_wifi_icon_now();
        clock_update_ble_icon_now();
        ui_set_watch_batt(g_watch_batt_pct, g_watch_batt_v);
        ui_notif_refresh_async();

    } else if (s == UI_SETTINGS) {
        if (scr_settings) {
            lv_obj_del(scr_settings);
            scr_settings = NULL;
        }
        scr_settings = ui_build_settings_screen();
        lv_scr_load(scr_settings);
        ui_update_ip_label();

    } else if (s == UI_LOG) {
        if (!scr_log) scr_log = ui_build_log_screen();
        lv_scr_load(scr_log);
        ui_log_screen_pause(false);

    } else if (s == UI_POWER_MENU) {                // <-- NEW
        if (!scr_power) scr_power = ui_build_power_menu();
        lv_scr_load(scr_power);

    } else if (s == UI_BLANK) {
        if (!scr_blank) scr_blank = ui_build_black_screen();
        lv_scr_load(scr_blank);
    }

    g_ui_current = s;
}

void create_watch_ui(void)
{
    ui_show(UI_CLOCK);
}
