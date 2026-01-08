#pragma once
#include <lvgl.h>
#include <stdbool.h>
#include "watch_globals.h"
#include "esp_log.h"
#include <lvgl.h>
#include <stdbool.h>

/* globals */
#include "watch_globals.h"

/* modules the UI calls into */
#include "watch_sleep.h"      // mark_user_activity, activity_event_cb
#include "watch_wifi.h"       // wifi_start_sta, wifi_stop, wifi_get_rssi_dbm, start_wifi_scan, wifi_forget_saved
#include "watch_settings.h"   // settings_save_*
#include "watch_time.h"       // set_system_time_hm
#include "watch_ble.h"        // ble_start, ble_stop
#include "watch_heartrate.h"  // hr_request_read_now, g_hr_period_ms, g_hr_run_ms
#include "watch_logbuf.h"

/* icons + fonts the UI uses */
#include "watch_icons/watch_icons.h"
#include "fonts/fonts.h"
/* display helpers you call */
#include "display.h"
#include "esp_bsp.h"

/* ---------------- UI lifecycle ---------------- */
void create_watch_ui(void);
void ui_show(ui_screen_t s);

/* ---------------- Clock helpers ---------------- */
void clock_update_label_now(void);
void clock_update_wifi_icon_now(void);
void clock_update_ble_icon_now(void);

/* ---------------- Battery / status ---------------- */
void ui_set_phone_batt(int pct, bool charging);
void ui_set_watch_batt(int pct, float volts_unused);

/* ---------------- Notifications ---------------- */
void ui_notif_add(notif_type_t t);
void ui_notif_clear_type(notif_type_t t);
void ui_notif_clear_all(void);
void ui_notif_add_from_ble(notif_type_t t);
void ui_notif_refresh_async(void);

/* ---------------- Modals ---------------- */
void open_wifi_picker_modal(lv_obj_t *parent);
void close_wifi_picker_modal(void);
void open_wifi_password_modal(lv_obj_t *parent, const char *ssid);
void close_wifi_password_modal(void);


/* ---------------- Tiles ---------------- */
lv_obj_t *tile_create_base(
    lv_obj_t *parent,
    const char *title,
    const char *subtitle,
    lv_obj_t **out_sub_lbl
);

lv_obj_t *tile_create_nav_tile(
    lv_obj_t *parent,
    const char *title,
    const char *subtitle,
    lv_event_cb_t on_click
);

lv_obj_t *tile_create_switch_tile(
    lv_obj_t *parent,
    const char *title,
    const char *subtitle,
    bool initial_on,
    lv_event_cb_t on_switch_value_changed,
    lv_obj_t **out_sub_lbl
);
void tile_set_on(lv_obj_t *btn, bool on);

/* ---------------- Screen builders (router-only) ---------------- */
lv_obj_t *ui_build_home_screen(void);
lv_obj_t *ui_build_clock_screen(void);
lv_obj_t *ui_build_settings_screen(void);
lv_obj_t *ui_build_blank_screen(void);
lv_obj_t *ui_build_log_screen(void);

/* ---------------- HR debug ---------------- */
void apply_backlight_percent(int pct);
void clock_timer_cb(lv_timer_t *t);
void ui_notif_bar_attach(lv_obj_t *clock_screen_parent);
void open_brightness_modal(lv_obj_t *parent);
void open_settime_modal(lv_obj_t *parent);
void ui_update_wifi_icon_async(void *arg);
void ui_update_ble_status_async(void *arg);
void ui_update_ble_icon_async(void *arg);
void ui_set_watch_batt_async(void *arg);
void ui_update_ip_label(void);
void ui_update_ip_label_async(void *arg);
void ui_update_clock_async(void *arg);
void clock_update_label_now(void);
lv_obj_t *ui_build_black_screen(void);
void on_wifi_ap_clicked(lv_event_t *e);
void ui_hr_widget_refresh_request(void);
void log_screen_refresh(lv_timer_t *t);
void ui_log_screen_pause(bool pause);
