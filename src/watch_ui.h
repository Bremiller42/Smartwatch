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
