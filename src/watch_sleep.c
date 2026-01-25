// FILE: watch_sleep.c

#include "watch_sleep.h"

#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

#include "watch_globals.h"   // g_last_activity_ms, g_screen_awake, g_ignore_until_ms, etc.
#include "ui_priv.h"         // ui_show(UI_BLANK), UI_CLOCK, etc.
#include "watch_power.h"
#include "watch_wifi.h"
#include "watch_ble.h"
#include "display.h"
#include "esp_lcd_touch.h"   // esp_lcd_touch_*
#include "esp_bsp.h"

static const char *TAG = "SLEEP_MGR";

/* Defaults (ms) — override from Settings */
uint32_t g_wifi_off_delay_ms = 2 * 60 * 1000;
uint32_t g_ble_slow_delay_ms = 3 * 60 * 1000;
uint32_t g_ble_off_delay_ms  = 8 * 60 * 1000;


/* Internals */
static lv_timer_t *s_mgr_timer = NULL;

static volatile bool s_touch_pending = false;
static uint32_t s_last_touch_read_ms  = 0;
static uint32_t s_last_listen_poll_ms = 0;
static esp_lcd_touch_handle_t s_tp = NULL;

/* Forced-off tracking (THIS is policy-state, not user intent) */
static bool s_wifi_forced_off = false;
static bool s_ble_forced_off  = false;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* ---------- Touch ISR ---------- */
static void tp_isr_cb(esp_lcd_touch_handle_t tp)
{
    (void)tp;
    s_touch_pending = true;
}

/* ---------- Screen actions (sleep mgr is the only place that touches these) ---------- */
static void screen_sleep_local(void)
{
    if (!g_screen_awake) return;

    g_screen_awake = false;
    ui_show(UI_BLANK);
    bsp_display_backlight_off();
    ESP_LOGI(TAG, "Screen -> OFF");
}

static void screen_wake_local(void)
{
    if (g_screen_awake) return;

    g_screen_awake = true;

    // Your wake landing screen
    ui_show(UI_CLOCK);
    ui_notif_refresh_async();
    
    apply_backlight_percent(g_brightness);
    ESP_LOGI(TAG, "Screen -> ON");
}

/* Touch polling cadence by stage */
static uint32_t poll_period_for_stage(sleep_stage_t st)
{
    switch (st) {
        case SLP_AWAKE:      return 20;
        case SLP_SCREEN_OFF: return 120;
        case SLP_WIFI_OFF:   return 250;
        case SLP_BLE_SLOW:   return 400;
        case SLP_BLE_OFF:    return 800;
        default:             return 250;
    }
}

/* ---------- Radio restore helpers (only restore what *we* forced off) ---------- */
static void maybe_restore_wifi(void)
{
    if (!s_wifi_forced_off) return;
    s_wifi_forced_off = false;

    // Only restore if user still wants WiFi ON
    if (!g_wifi_on) return;

    // Only restore if we have creds
    if (g_wifi_ssid[0] == '\0') return;

    // Bring stack up and connect
    wifi_ensure_started();
    (void)wifi_start_sta(g_wifi_ssid, g_wifi_pass);

    ESP_LOGI(TAG, "WiFi -> restore (after forced-off)");
}

static void maybe_restore_ble(void)
{
    if (!s_ble_forced_off) return;
    s_ble_forced_off = false;

    // Only restore if user still has BLE enabled (owner is BLE module now)
    if (!ble_is_enabled()) return;

    ble_start();
    ESP_LOGI(TAG, "BLE -> restore (after forced-off)");
}

/* ---------- Stage transitions ---------- */
static void enter_stage(sleep_stage_t st)
{
    if (g_sleep_stage == st) return;

    ESP_LOGI(TAG, "Stage %d -> %d", (int)g_sleep_stage, (int)st);
    g_sleep_stage = st;

    switch (st) {

        case SLP_AWAKE:
            screen_wake_local();

            (void)watch_power_set_profile_awake();
            ble_request_awake_params();

            // Restore only what we forced off (and only if user still wants it)
            maybe_restore_wifi();
            maybe_restore_ble();
            break;

        case SLP_SCREEN_OFF:
            screen_sleep_local();
            (void)watch_power_set_profile_sleep();
            ble_request_sleep_params();
            break;

        case SLP_WIFI_OFF:
            // Only force WiFi off if user wants it on (otherwise settings already owns it)
            if (g_wifi_on) {
                // Don’t spam stop calls; record that this was a policy action
                if (!s_wifi_forced_off) s_wifi_forced_off = true;
                wifi_stop();
                ESP_LOGI(TAG, "WiFi -> OFF (forced by sleep stage)");
            }
            break;

        case SLP_BLE_SLOW:
            // Only meaningful if connected; function already no-ops if not connected
            if (ble_is_enabled()) {
                ble_request_sleep_params();
            }
            break;

        case SLP_BLE_OFF:
            // Only force BLE off if user has BLE enabled.
            // If user disabled BLE, we should NOT touch it (or mark forced-off).
            if (ble_is_enabled()) {
                if (!s_ble_forced_off) s_ble_forced_off = true;
                ble_stop();
                ESP_LOGI(TAG, "BLE -> OFF (forced by sleep stage)");
            }
            break;
    }
}

/* ---------- Touch service (ISR flag + fallback polling) ---------- */
static void service_touch(uint32_t tnow)
{
    if (!s_tp) return;

    const uint32_t min_read_gap_ms = 25;
    bool should_poll = false;

    if (s_touch_pending) {
        s_touch_pending = false;
        should_poll = true;
    } else {
        uint32_t period = poll_period_for_stage(g_sleep_stage);
        if (tnow - s_last_listen_poll_ms >= period) {
            s_last_listen_poll_ms = tnow;
            should_poll = true;
        }
    }

    if (!should_poll) return;
    if (tnow - s_last_touch_read_ms < min_read_gap_ms) return;
    s_last_touch_read_ms = tnow;

    (void)esp_lcd_touch_read_data(s_tp);

    uint16_t x[1] = {0}, y[1] = {0}, s[1] = {0};
    uint8_t n = 0;

    bool touched = esp_lcd_touch_get_coordinates(s_tp, x, y, s, &n, 1);

    if (touched && n > 0) {
        g_last_activity_ms = tnow;

        if (g_sleep_stage != SLP_AWAKE) {
            enter_stage(SLP_AWAKE);
            g_ignore_until_ms = tnow + 250;
            g_require_release_after_wake = true;
        }
    }
}


/* ---------- LVGL timer callback ---------- */
static void manager_cb(lv_timer_t *t)
{
    (void)t;

    const uint32_t tnow = now_ms();
    const uint32_t idle = tnow - g_last_activity_ms;

    // Stage progression
    if (idle >= g_ble_off_delay_ms) {
        enter_stage(SLP_BLE_OFF);
    } else if (idle >= g_ble_slow_delay_ms) {
        enter_stage(SLP_BLE_SLOW);
    } else if (idle >= g_wifi_off_delay_ms) {
        enter_stage(SLP_WIFI_OFF);
    } else if (idle >= g_screen_timeout_ms) {
        enter_stage(SLP_SCREEN_OFF);
    } else {
        enter_stage(SLP_AWAKE);
    }

    // Always service touch; stage controls cadence
    service_touch(tnow);
}

/* ---------- Public API (your project already calls these) ---------- */
esp_err_t watch_sleep_init(void)
{
    g_last_activity_ms = now_ms();

    s_tp = bsp_display_get_touch();
    if (!s_tp) {
        ESP_LOGW(TAG, "Touch handle NULL; wake will be polling-only");
    }

    if (!s_mgr_timer) {
        s_mgr_timer = lv_timer_create(manager_cb, 50, NULL);
    }

    ESP_LOGI(TAG, "Sleep manager init OK");
    return ESP_OK;
}
void IRAM_ATTR watch_sleep_touch_irq_hint_from_isr(void)
{
    s_touch_pending = true;
}

void watch_sleep_notify_activity(void)
{
    const uint32_t tnow = now_ms();
    g_last_activity_ms = tnow;

    if (g_sleep_stage != SLP_AWAKE) {
        enter_stage(SLP_AWAKE);
        g_ignore_until_ms = tnow + 250;
        g_require_release_after_wake = true;
    }
}

void watch_sleep_force_awake(void)
{
    watch_sleep_notify_activity();
}

void watch_sleep_force_screen_off(void)
{
    enter_stage(SLP_SCREEN_OFF);
}
