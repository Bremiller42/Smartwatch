// FILE: src/watch_shutdown.c
#include "watch_shutdown.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_attr.h"   // RTC_DATA_ATTR

#include "watch_globals.h"
#include "watch_power.h"
#include "watch_settings.h"
#include "watch_wifi.h"
#include "watch_ble.h"
#include "watch_backlight.h"
#include "watch_screen_timeout.h"
#include "lvgl.h"
#include "ui_priv.h"

static const char *TAG = "CRIT_PWR";

typedef struct {
    float low_v;
    float low_exit_v;
    float critical_v;
    float critical_exit_v;
    float shutdown_v;
    int   shutdown_confirm_ms;
    int   min_transition_ms;
    int   cap_pct_low;
    int   cap_pct_critical;
} shdn_cfg_t;

static shdn_cfg_t s_cfg = {
    .low_v               = 3.65f,
    .low_exit_v          = 3.75f,

    .critical_v          = 3.50f,
    .critical_exit_v     = 3.60f,

    .shutdown_v          = 3.35f,
    .shutdown_confirm_ms = 8000,

    .min_transition_ms   = 2000,

    // These are CAPS now, not “set brightness”
    .cap_pct_low         = 35,
    .cap_pct_critical    = 10,
};

static bool s_ready = false;
float watch_shutdown_get_critical_v(void) { return s_cfg.critical_v; }

static shdn_state_t s_state = SHDN_OK;
static int64_t s_below_shutdown_since_ms = -1;
static int64_t s_last_transition_ms = 0;
static bool s_actions_applied_low = false;
static bool s_actions_applied_critical = false;

/* ✅ RTC latch: survives deep sleep without flash/NVS writes */
RTC_DATA_ATTR static uint8_t s_low_pwr_latch = 0;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static bool can_transition(void) { return (now_ms() - s_last_transition_ms) > s_cfg.min_transition_ms; }

/* ---------------- Public helpers ---------------- */
bool watch_shutdown_is_ready(void)
{
    return s_ready;
}

void watch_shutdown_set_low_power_latch(bool on)
{
    s_low_pwr_latch = on ? 1 : 0;
}

bool watch_shutdown_low_power_latched(void)
{
    return (s_low_pwr_latch != 0);
}

/* ---------------- UI helpers ---------------- */
static void ui_warn_async(void *arg)
{
    const char *msg = (const char *)arg;
    (void)msg;
    // You can show msg via a toast/label later
}

static void ui_show_low_pwr_async(void *arg)
{
    (void)arg;
    ui_show(UI_LOW_PWR);
}

/* ---------------- Actions ---------------- */
static void apply_low_actions(void)
{
    if (s_actions_applied_low) return;
    s_actions_applied_low = true;

    ESP_LOGW(TAG, "LOW battery actions (cap=%d%%)", s_cfg.cap_pct_low);

    // ✅ Cap brightness (does NOT change user preference)
    backlight_set_cap_pct(s_cfg.cap_pct_low);

    (void)watch_power_set_profile_sleep();
    lv_async_call(ui_warn_async, (void *)"LOW BATTERY");
}

static void apply_critical_actions(void)
{
    if (s_actions_applied_critical) return;
    s_actions_applied_critical = true;

    /* ✅ latch critical UI state */
    watch_shutdown_set_low_power_latch(true);

    ESP_LOGE(TAG, "CRITICAL battery actions (cap=%d%%)", s_cfg.cap_pct_critical);

    // ✅ Cap brightness more aggressively
    backlight_set_cap_pct(s_cfg.cap_pct_critical);
    // ✅ Make sure timeout system can actually shut screen back off
    screen_keep_awake_set(false);        // hard clear (refs->0)
    screen_timeout_set_always_on(false); // just in case user had it on
    /* ✅ show low power screen as “last screen” */
    lv_async_call(ui_show_low_pwr_async, NULL);

    ESP_LOGW(TAG, "Emergency disabling Wi-Fi/BLE (critical)");
    wifi_stop();
    ble_set_enabled(false);

    ESP_LOGW(TAG, "Unmount SD to prevent write current spikes");
    lv_async_call(ui_warn_async, (void *)"CRITICAL BATTERY");
}

static void prepare_for_sleep(void)
{
    ESP_LOGW(TAG, "Preparing for deep sleep");
    settings_commit_dirty_now(); // already gated in settings
}

static void enter_deep_sleep(void)
{
    ESP_LOGW(TAG, "Entering deep sleep due to low VBAT");
    esp_deep_sleep_start();
}

bool watch_shutdown_storage_allowed(void)
{
    if (watch_shutdown_low_power_latched()) return false;
    return (s_state == SHDN_OK || s_state == SHDN_LOW);
}

/* ---------------- API ---------------- */
esp_err_t watch_shutdown_init(void)
{
    s_state = SHDN_OK;
    s_below_shutdown_since_ms = -1;
    s_last_transition_ms = 0;
    s_actions_applied_low = false;
    s_actions_applied_critical = false;

    s_ready = true;

    ESP_LOGI(TAG, "Shutdown controller initialized (latch=%u)", (unsigned)s_low_pwr_latch);
    return ESP_OK;
}

shdn_state_t watch_shutdown_state(void)
{
    return s_state;
}

void watch_shutdown_update(float vbat, bool screen_awake)
{
    (void)screen_awake;
    if (vbat <= 0.0f) return;

    int64_t t = now_ms();

    if (vbat <= s_cfg.shutdown_v) {
        if (s_below_shutdown_since_ms < 0) s_below_shutdown_since_ms = t;
    } else {
        s_below_shutdown_since_ms = -1;
    }

    switch (s_state) {
        case SHDN_OK:
            if (!can_transition()) break;

            if (vbat <= s_cfg.critical_v) {
                s_state = SHDN_CRITICAL;
                s_last_transition_ms = t;
                apply_critical_actions();
            } else if (vbat <= s_cfg.low_v) {
                s_state = SHDN_LOW;
                s_last_transition_ms = t;
                apply_low_actions();
            } else {
                // Normal: ensure cap is relaxed
                backlight_set_cap_pct(100);
            }
            break;

        case SHDN_LOW:
            if (can_transition() && vbat >= s_cfg.low_exit_v) {
                ESP_LOGI(TAG, "Battery recovered -> OK");
                s_state = SHDN_OK;
                s_last_transition_ms = t;

                s_actions_applied_low = false;
                s_actions_applied_critical = false;

                watch_shutdown_set_low_power_latch(false);

                // ✅ restore cap on recovery
                backlight_set_cap_pct(100);
                break;
            }

            if (can_transition() && vbat <= s_cfg.critical_v) {
                s_state = SHDN_CRITICAL;
                s_last_transition_ms = t;
                apply_critical_actions();
            }
            break;

        case SHDN_CRITICAL:
            if (can_transition() && vbat >= s_cfg.critical_exit_v) {
                ESP_LOGI(TAG, "Recovered -> LOW");
                s_state = SHDN_LOW;
                s_last_transition_ms = t;

                watch_shutdown_set_low_power_latch(false);

                // ✅ relax cap up to LOW cap (still protective)
                backlight_set_cap_pct(s_cfg.cap_pct_low);
            }

            if (s_below_shutdown_since_ms > 0 &&
                (t - s_below_shutdown_since_ms) >= s_cfg.shutdown_confirm_ms)
            {
                s_state = SHDN_SHUTTING_DOWN;
                s_last_transition_ms = t;
                prepare_for_sleep();
                enter_deep_sleep();
            }
            break;

        case SHDN_SHUTTING_DOWN:
        default:
            break;
    }
}
