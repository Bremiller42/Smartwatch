// FILE: src/ui/watch_ui.c
#include "ui_priv.h"
#include "esp_log.h"
#include "watch_globals.h"
#include "watch_fuel.h"
#include "watch_shutdown.h"
const char *UI_TAG = "UI";

void ui_show(ui_screen_t s)
{
    // Block most transitions while critical/shutting down
    shdn_state_t st = watch_shutdown_state();
    if ((st == SHDN_CRITICAL || st == SHDN_SHUTTING_DOWN) &&
        (s != UI_LOW_PWR && s != UI_BLANK)) {
        ESP_LOGW(UI_TAG, "UI blocked during CRITICAL (req=%d)", s);
        s = UI_LOW_PWR;
    }

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
    else if (s == UI_LOW_PWR) {
        if (!scr_low_pwr) scr_low_pwr = ui_build_low_pwr_screen();
        lv_scr_load(scr_low_pwr);
    }
    else if (s == UI_DEVICE_INFO) {
        if (ui_device_info) {
            lv_obj_del(ui_device_info);
            ui_device_info = NULL;
        }
        ui_device_info = ui_build_device_info_screen();
        lv_scr_load(ui_device_info);
    }
    else if (s == UI_SMS_OVERLAY) {
        // Always rebuild overlay so it shows the freshest "latest" message
        if (scr_sms_overlay) {
            lv_obj_del(scr_sms_overlay);
            scr_sms_overlay = NULL;
        }
        scr_sms_overlay = ui_build_sms_overlay_screen();
        lv_scr_load(scr_sms_overlay);
    }
    else if (s == UI_SMS_THREADS) {
        // Rebuild so the list reflects new log data
        if (scr_sms_threads) {
            lv_obj_del(scr_sms_threads);
            scr_sms_threads = NULL;
        }
        scr_sms_threads = ui_build_sms_threads_screen();
        lv_scr_load(scr_sms_threads);
    }
    else if (s == UI_SMS_THREAD_VIEW) {
        // Rebuild because active thread changes and messages may update
        if (scr_sms_thread) {
            lv_obj_del(scr_sms_thread);
            scr_sms_thread = NULL;
        }
        scr_sms_thread = ui_build_sms_thread_view_screen();
        lv_scr_load(scr_sms_thread);
    }


    g_ui_current = s;
}

void create_watch_ui(void)
{
    ui_show(UI_CLOCK);
}
