#pragma once

#include <lvgl.h>
#include <stdbool.h>
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_wifi_types.h"

/* ---------------- UI globals ---------------- */
extern lv_obj_t *clock_date_lbl;
extern lv_obj_t *clock_time_lbl;
extern lv_timer_t *clock_timer;

extern bool use_24h_format;
extern bool g_time_synced;

extern int g_brightness;
extern lv_obj_t *brightness_slider;

// WIFI icon + IP label (shown in UI)
extern lv_obj_t *clock_wifi_icon;
extern lv_obj_t *ip_lbl;

// BLE icon
extern lv_obj_t *clock_ble_icon;


/* Set-time dialog state */
extern lv_obj_t *settime_modal;
extern lv_obj_t *settime_h_lbl;
extern lv_obj_t *settime_m_lbl;
extern int set_h;
extern int set_m;

/* ---------------- Wi-Fi ---------------- */
extern EventGroupHandle_t s_wifi_evgrp;
extern esp_netif_t *s_sta_netif;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define KEY_WIFI_SSID   "wifissid"
#define KEY_WIFI_PASS   "wifipass"

#define KEY_SCREEN_TIMEOUT "scto"   // u32 (store seconds or ms)


extern char g_wifi_ssid[33];
extern char g_wifi_pass[65];

extern int s_retry_num;
extern const int WIFI_MAX_RETRY;
extern char g_ip_str[16];

extern bool s_netif_inited;
extern bool s_event_loop_inited;

extern lv_obj_t *wifi_modal;
extern lv_obj_t *wifi_list;
extern lv_obj_t *wifi_status_lbl;
extern lv_obj_t *wifi_pass_modal;
extern lv_obj_t *wifi_pass_ta;
extern lv_obj_t *wifi_kb;

extern bool g_wifi_on;
extern bool g_wifi_connected;

extern bool g_scan_in_progress;
extern char g_selected_ssid[33];

extern bool s_wifi_inited;
extern bool s_handlers_registered;
extern esp_event_handler_instance_t s_wifi_any_id_inst;
extern esp_event_handler_instance_t s_got_ip_inst;


/* -----------------BLE----------------- */
extern bool g_ble_on;
extern bool g_ble_connected;

extern char g_ble_peer[32];   // if you later want peer name/address


/* ---------------- Sleep / Wake ---------------- */
#define SCREEN_TIMEOUT_MS   (15 * 1000)
#define WAKE_DEBOUNCE_MS    250

extern lv_timer_t *sleep_timer;
extern uint32_t g_last_activity_ms;
extern bool g_screen_awake;
extern lv_timer_t *touch_activity_timer;

extern uint32_t g_ignore_until_ms;
extern bool g_require_release_after_wake;

extern lv_obj_t *wake_blocker;
extern bool g_blocker_active;

extern uint32_t g_screen_timeout_ms;

/* ---------------- NVS Settings ---------------- */
#include "nvs.h"

extern nvs_handle_t g_nvs;
extern bool g_nvs_ok;

#define NVS_NS          "watch"
#define KEY_BRIGHTNESS  "bright"
#define KEY_USE_24H     "use24h"
#define KEY_WIFI_ON     "wifion"
#define KEY_LAST_EPOCH  "lastepoch"
#define KEY_LAST_US     "lastus"
#define KEY_BLE_ON "ble_on"

/* ---------------- UI screen pointers ---------------- */
typedef enum {
    UI_HOME = 0,
    UI_CLOCK = 1,
    UI_SETTINGS = 2,
    UI_BLANK = 3,
} ui_screen_t;

extern lv_obj_t *scr_blank;
extern lv_obj_t *scr_home;
extern lv_obj_t *scr_clock;
extern lv_obj_t *scr_settings;
extern lv_obj_t *ble_status_lbl;   // optional
extern lv_obj_t *clock_notification_icon_box;
/* ---------------- Rotation ---------------- */
#define LVGL_PORT_ROTATION_DEGREE (90)

typedef enum {
    NOTIF_SMS = 0,
    NOTIF_EMAIL,
    NOTIF_MSG,
    NOTIF_APP,
    NOTIF_MAX
} notif_type_t;

extern uint16_t g_notif_counts[NOTIF_MAX];

extern lv_obj_t *notif_icon_row;
extern lv_obj_t *notif_icon_lbl[NOTIF_MAX];
extern lv_obj_t *notif_badge_lbl[NOTIF_MAX];

extern volatile bool g_notif_dirty;
