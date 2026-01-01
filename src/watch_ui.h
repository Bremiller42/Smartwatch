#pragma once
#include <lvgl.h>
#include "watch_globals.h"

/* Backlight + core UI helpers */
void apply_backlight_percent(int pct);

void ui_update_ip_label(void);
void clock_update_label_now(void);
void clock_update_wifi_icon_now(void);

/* Async wrappers used by WiFi / Time callbacks */
void ui_update_ip_label_async(void *arg);
void ui_update_clock_async(void *arg);
void ui_update_wifi_icon_async(void *arg);

/* Screen router + builder */
void ui_show(ui_screen_t s);
void create_watch_ui(void);

/* Modals exposed for WiFi module toggles */
void open_wifi_picker_modal(lv_obj_t *parent);

/* used by WiFi module */
void close_wifi_picker_modal(void);
void open_wifi_password_modal(lv_obj_t *parent, const char *ssid);
void close_wifi_password_modal(void);
void on_wifi_ap_clicked(lv_event_t *e);

void clock_update_ble_icon_now(void);
void ui_update_ble_icon_async(void *arg);
void ui_update_ble_status_async(void *arg);
// watch_ui.h
void ui_notif_add_from_ble(notif_type_t t);
void ui_notif_refresh_async(void);
extern lv_obj_t *phone_batt_lbl;
extern int g_phone_batt_pct;
extern bool g_phone_batt_charging;
void ui_set_phone_batt(int pct, bool charging);
void ui_clear_phone_batt(void);
lv_obj_t* ui_get_hr_debug_lbl(void);
