#include "watch_globals.h"
#include "watch_sleep.h"

/* ---------------- TAG ---------------- */
const char *TAG = "GLOBALS";

/* ---------------- UI globals ---------------- */
lv_obj_t *clock_date_lbl = NULL;
lv_obj_t *clock_time_lbl = NULL;
lv_timer_t *clock_timer  = NULL;

bool use_24h_format = false;
bool g_time_synced = false;

int g_brightness = 70;
lv_obj_t *brightness_slider = NULL;


lv_obj_t *clock_wifi_icon = NULL;
lv_obj_t *ip_lbl = NULL;

lv_obj_t *clock_ble_icon = NULL;
lv_obj_t *clock_notification_icon_box = NULL;
/* Set-time dialog state */
lv_obj_t *settime_modal = NULL;
lv_obj_t *settime_h_lbl = NULL;
lv_obj_t *settime_m_lbl = NULL;
int set_h = 12;
int set_m = 0;


lv_obj_t *phone_batt_lbl = NULL;
/* ---------------- Wi-Fi ---------------- */
EventGroupHandle_t s_wifi_evgrp = NULL;
esp_netif_t *s_sta_netif = NULL;

char g_wifi_ssid[33] = "";
char g_wifi_pass[65] = "";

int s_retry_num = 0;
const int WIFI_MAX_RETRY = 10;

char g_ip_str[16] = "0.0.0.0";

bool s_netif_inited = false;
bool s_event_loop_inited = false;

lv_obj_t *wifi_modal = NULL;
lv_obj_t *wifi_list = NULL;
lv_obj_t *wifi_status_lbl = NULL;
lv_obj_t *wifi_pass_modal = NULL;
lv_obj_t *wifi_pass_ta = NULL;
lv_obj_t *wifi_kb = NULL;

bool g_wifi_on = true;
bool g_wifi_connected = false;

bool g_scan_in_progress = false;
char g_selected_ssid[33] = "";

bool s_wifi_inited = false;
bool s_handlers_registered = false;
esp_event_handler_instance_t s_wifi_any_id_inst;
esp_event_handler_instance_t s_got_ip_inst;

/* -----------------BLE----------------- */
bool g_ble_on = true;         // default ON if you want, or false
bool g_ble_connected = false;
char g_ble_peer[32] = {0};
lv_obj_t *ble_status_lbl = NULL;

/* ---------------- Sleep / Wake ---------------- */
lv_timer_t *sleep_timer = NULL;
uint32_t g_last_activity_ms = 0;
lv_timer_t *touch_activity_timer = NULL;

uint32_t g_ignore_until_ms = 0;
bool g_require_release_after_wake = false;

lv_obj_t *wake_blocker = NULL;
bool g_blocker_active = false;

uint32_t g_screen_timeout_ms = 15000; // default 15 seconds

volatile bool g_screen_awake = true;

/* ---------------- NVS Settings ---------------- */
nvs_handle_t g_nvs = 0;
bool g_nvs_ok = false;

/* ---------------- UI screen pointers ---------------- */
lv_obj_t *scr_home     = NULL;
lv_obj_t *scr_clock    = NULL;
lv_obj_t *scr_settings = NULL;
lv_obj_t *scr_blank    = NULL;
lv_obj_t *scr_log      = NULL;

uint16_t g_notif_counts[NG_MAX] = {0};

lv_obj_t *notif_icon_row = NULL;
lv_obj_t *notif_icon_lbl[NG_MAX] = {0};
lv_obj_t *notif_badge_lbl[NG_MAX] = {0};
lv_obj_t *badge_outline[NG_MAX] = {0};
lv_obj_t *watch_batt_lbl = NULL;
volatile bool g_notif_dirty = false;
ui_screen_t g_ui_current = (ui_screen_t)-1;

volatile sleep_stage_t g_sleep_stage = SLP_AWAKE;
