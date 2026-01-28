// FILE: watch_sleep.c
//
// Power-focused sleep manager for LVGL8 + ESP32-S3 smartwatch
// Fixes:
// - Screen timeout applies regardless of radio stage
// - Screen state owned ONLY here (no double-toggle via enter_stage)
// - Touch wake uses IRQ hint
//
// Brightness policy update:
// - Sleep manager does NOT change user brightness
// - On wake: re-apply effective brightness via backlight manager (min(user, cap))
// - On sleep: BSP backlight is turned off (0% PWM)

#include "watch_sleep.h"

#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

#include "watch_globals.h"
#include "ui_priv.h"
#include "watch_power.h"
#include "watch_wifi.h"
#include "watch_ble.h"
#include "display.h"
#include "esp_lcd_touch.h"
#include "esp_bsp.h"
#include "watch_screen_timeout.h"
#include "watch_backlight.h"   // ✅ NEW

static const char *TAG = "SLEEP_MGR";

static volatile uint32_t s_touch_irq_count = 0;

/* Defaults (ms) — override from Settings */
uint32_t g_wifi_off_delay_ms = 2 * 60 * 1000;
uint32_t g_ble_slow_delay_ms = 3 * 60 * 1000;
uint32_t g_ble_off_delay_ms  = 8 * 60 * 1000;

/* Internals */
static lv_timer_t *s_mgr_timer = NULL;

static volatile bool s_touch_pending = false;
static uint32_t s_last_touch_read_ms  = 0; // reserved if you later add “read once” debounce
static esp_lcd_touch_handle_t s_tp = NULL;

/* Forced-off tracking (policy state, not user intent) */
static bool s_wifi_forced_off = false;
static bool s_ble_forced_off  = false;
static lv_disp_t *s_disp = NULL;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* ---------- Manager tick period adaptation ---------- */
static void manager_adjust_period(void)
{
    if (!s_mgr_timer) return;
    lv_timer_set_period(s_mgr_timer, g_screen_awake ? 50 : 300); // tune if needed
}

/* ---------- Screen actions (owned ONLY here) ---------- */
static void screen_sleep_local(void)
{
    if (!g_screen_awake) return;
    g_screen_awake = false;

    // Swap to blank screen while still “awake” to avoid weird residual artifacts
    bsp_display_lock(0);
    ui_show(UI_BLANK);
    lv_refr_now(NULL);
    bsp_display_unlock();

    // Hard off: 0% PWM (does not modify user preference or cap)
    backlight_set_screen_on(false);

    manager_adjust_period();
    ESP_LOGI(TAG, "Screen -> OFF (backlight off)");
}

static void screen_wake_local(void)
{
    if (g_screen_awake) return;
    g_screen_awake = true;

    bsp_display_lock(0);
    ui_show(UI_CLOCK);
    lv_refr_now(NULL);
    bsp_display_unlock();

    // Refresh notifications (if you want)
    ui_notif_refresh_async();

    // ✅ Re-apply effective brightness (min(user, cap)) without changing user preference
    backlight_set_screen_on(true);

    manager_adjust_period();
    ESP_LOGI(TAG, "Screen -> ON (brightness eff=%d user=%d cap=%d)",
             backlight_get_effective_pct(),
             backlight_get_user_pct(),
             backlight_get_cap_pct());
}

/* ---------- Radio restore helpers (only restore what *we* forced off) ---------- */
static void maybe_restore_wifi(void)
{
    if (!s_wifi_forced_off) return;
    s_wifi_forced_off = false;

    if (!g_wifi_on) return;
    if (g_wifi_ssid[0] == '\0') return;

    wifi_ensure_started();
    (void)wifi_start_sta(g_wifi_ssid, g_wifi_pass);
    ESP_LOGI(TAG, "WiFi -> restore (after forced-off)");
}

static void maybe_restore_ble(void)
{
    if (!s_ble_forced_off) return;
    s_ble_forced_off = false;

    if (!ble_is_enabled()) return;

    ble_start();
    ESP_LOGI(TAG, "BLE -> restore (after forced-off)");
}

/* ---------- Stage transitions (RADIO/PERF ONLY; NO SCREEN TOGGLES HERE) ---------- */
static void enter_stage(sleep_stage_t st)
{
    if (g_sleep_stage == st) return;

    ESP_LOGI(TAG, "Stage %d -> %d", (int)g_sleep_stage, (int)st);
    g_sleep_stage = st;

    switch (st) {
        case SLP_AWAKE:
            (void)watch_power_set_profile_awake();
            ble_request_awake_params();
            maybe_restore_wifi();
            maybe_restore_ble();
            break;

        case SLP_SCREEN_OFF:
            (void)watch_power_set_profile_sleep();
            ble_request_sleep_params();
            break;

        case SLP_WIFI_OFF:
            if (g_wifi_on) {
                if (!s_wifi_forced_off) s_wifi_forced_off = true;
                wifi_stop();
                ESP_LOGI(TAG, "WiFi -> OFF (forced by sleep stage)");
            }
            break;

        case SLP_BLE_SLOW:
            if (ble_is_enabled()) {
                ble_request_sleep_params();
            }
            break;

        case SLP_BLE_OFF:
            if (ble_is_enabled()) {
                if (!s_ble_forced_off) s_ble_forced_off = true;
                ble_stop();
                ESP_LOGI(TAG, "BLE -> OFF (forced by sleep stage)");
            }
            break;

        default:
            break;
    }
}

/* ---------- Touch service (IRQ-driven when screen is off) ---------- */
static void service_touch(uint32_t tnow)
{
    if (!s_tp) return;

    // Did we get a touch IRQ hint since last tick?
    bool had_irq = s_touch_pending;
    if (had_irq) {
        s_touch_pending = false;
        g_last_activity_ms = tnow;   // treat IRQ as activity
    }

    // Only wake from screen-off if we actually got a new IRQ
    if (!g_screen_awake && had_irq) {
        screen_wake_local();
        enter_stage(SLP_AWAKE);

        g_ignore_until_ms = tnow + 250;
        g_require_release_after_wake = true;
    }

    // When awake: LVGL handles reading coords, no work needed here.
}

/* ---------- LVGL timer callback ---------- */
static void manager_cb(lv_timer_t *t)
{
    (void)t;

    const uint32_t tnow = now_ms();
    const uint32_t idle = tnow - g_last_activity_ms;

    // Screen policy ALWAYS applies
    const uint32_t to = screen_timeout_get_effective_ms();
    const bool should_screen_off = (to != 0 && idle >= to);

    if (should_screen_off) screen_sleep_local();
    else                  screen_wake_local();

    // Radio policy (independent)
    if (idle >= g_ble_off_delay_ms)       enter_stage(SLP_BLE_OFF);
    else if (idle >= g_ble_slow_delay_ms) enter_stage(SLP_BLE_SLOW);
    else if (idle >= g_wifi_off_delay_ms) enter_stage(SLP_WIFI_OFF);
    else                                  enter_stage(SLP_AWAKE);

    // Touch service may wake us
    service_touch(tnow);

    // Ensure timer period matches current screen state
    manager_adjust_period();

    // Optional refresh kick to avoid “stuck black” if awake
    if (g_screen_awake) {
        static uint32_t last_kick = 0;
        if (tnow - last_kick > 1000) {
            last_kick = tnow;
            bsp_display_lock(0);
            lv_refr_now(NULL);
            bsp_display_unlock();
        }
    }

    // Debug (optional)
    // static uint32_t last_dbg = 0;
    // if (tnow - last_dbg > 2000) {
    //     last_dbg = tnow;
    //     ESP_LOGI(TAG, "mgr alive idle=%u screen=%d touch_irq=%u pending=%d eff=%d user=%d cap=%d",
    //             (unsigned)idle, (int)g_screen_awake,
    //             (unsigned)s_touch_irq_count, (int)s_touch_pending,
    //             backlight_get_effective_pct(),
    //             backlight_get_user_pct(),
    //             backlight_get_cap_pct());
    // }
}

/* ---------- Public API ---------- */
esp_err_t watch_sleep_init(void)
{
    g_last_activity_ms = now_ms();
    s_disp = lv_disp_get_default();
    (void)s_disp;

    // Use the BSP touch handle (created in display init)
    s_tp = bsp_display_get_touch();
    if (!s_tp) {
        ESP_LOGW(TAG, "Touch handle NULL; wake will not work via touch");
    }

    if (!s_mgr_timer) {
        s_mgr_timer = lv_timer_create(manager_cb, 50, NULL);
    }
    manager_adjust_period();

    ESP_LOGI(TAG, "Sleep manager init OK");
    return ESP_OK;
}

void IRAM_ATTR watch_sleep_touch_irq_hint_from_isr(void)
{
    s_touch_pending = true;
    s_touch_irq_count++;
}

void watch_sleep_notify_activity(void)
{
    const uint32_t tnow = now_ms();
    g_last_activity_ms = tnow;

    // Ensure we are awake immediately
    screen_wake_local();
    enter_stage(SLP_AWAKE);

    g_ignore_until_ms = tnow + 250;
    g_require_release_after_wake = true;
}

void watch_sleep_force_awake(void)
{
    watch_sleep_notify_activity();
}

void watch_sleep_force_screen_off(void)
{
    // Force screen off immediately + apply sleep perf/radio profile
    screen_sleep_local();
    enter_stage(SLP_SCREEN_OFF);
}
