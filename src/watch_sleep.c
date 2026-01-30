// FILE: watch_sleep.c
//
// Power-focused sleep manager for LVGL8 + ESP32-S3 smartwatch
//
// POLICY (FINAL):
// - Screen power is independent and ALWAYS enforced by timeout logic.
// - When screen turns OFF we PAUSE LVGL + TE + panel + backlight in a safe order.
// - Performance profile follows screen state ONLY (never bounces awake while screen off).
// - Radio staging is independent and based on idle time.
// - When Wi-Fi is forced OFF, we pause network-driven timers (time/weather); restore on wake.
// - Battery state (watch_shutdown) can CLAMP/OVERRIDE screen timeout + radio restores.
//
// Screen sleep sequence (ORDER):
//   lvgl_port_pause()
//   bsp_display_te_pause()  (also gates sync now)
//   bsp_display_panel_on(false)
//   backlight off
//
// Wake sequence (ORDER):
//   bsp_display_panel_on(true)   (NOTE: your BSP function behaves like a TOGGLE; see below)
//   bsp_display_te_resume() (ungates sync now)
//   lvgl_port_resume()
//   backlight on
//   lv_refr_now(NULL) under lock (one forced refresh)
//
// NOTE ABOUT bsp_display_panel_on():
// You commented that passing "false" is required because the BSP function acts like a toggle.
// This file respects that and NEVER assumes true/false semantics. Instead we call it exactly
// as you requested (false on wake, false on sleep), and we only rely on g_screen_awake as the
// canonical state.

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
#include "esp_lcd_touch.h"
#include "esp_bsp.h"
#include "watch_weather.h"
#include "watch_screen_timeout.h"
#include "watch_backlight.h"
#include "lv_port.h"          // lvgl_port_pause/resume
#include "ui/ui_priv.h"
// Battery policy integration
#include "watch_shutdown.h"

static const char *TAG = "SLEEP_MGR";

/* Defaults (ms) — override from Settings */
uint32_t g_wifi_off_delay_ms = 2 * 60 * 1000;
uint32_t g_ble_slow_delay_ms = 3 * 60 * 1000;
// uint32_t g_ble_off_delay_ms  = 8 * 60 * 1000; // future night mode
static bool s_screen_forced_off_by_timeout = false;

/* Optional deep-sleep (compile-time) */
// #define WATCH_SLEEP_ENABLE_DEEP 1
#ifdef WATCH_SLEEP_ENABLE_DEEP
#include "esp_sleep.h"
static uint32_t g_deep_sleep_delay_ms = 20 * 60 * 1000; // 20 min default
// You MUST provide wake config elsewhere if you enable deep sleep.
// Example options:
//   esp_sleep_enable_ext0_wakeup(GPIO_NUM_x, 0/1);
//   esp_sleep_enable_ext1_wakeup(1ULL<<GPIO_NUM_x, ESP_EXT1_WAKEUP_ANY_HIGH);
#endif

static TaskHandle_t s_sleep_task = NULL;

static bool s_ao_wanted_enabled   = true;
static bool s_ao_wanted_force_off = false;
static bool s_ao_async_pending    = false;
/* Internals */
static volatile bool s_touch_pending = false;
static volatile uint32_t s_touch_irq_count = 0;

static esp_lcd_touch_handle_t s_tp = NULL;

/* Forced-off tracking (policy state, not user intent) */
static bool s_wifi_forced_off = false;
// static bool s_ble_forced_off  = false;

/* Independent state:
 * - perf state: awake vs sleep (driven ONLY by screen state)
 * - radio stage: wifi/ble policy actions driven by idle thresholds
 */
static bool s_perf_awake = true;
static sleep_stage_t s_radio_stage = SLP_AWAKE;

/* Track whether we paused LVGL for screen-off */
static bool s_lvgl_paused = false;

static void manager_cb(lv_timer_t *t);

typedef struct {
    bool enabled;
    bool force_off;
} ui_ao_args_t;

static void ui_set_always_on_tile_async(void *arg)
{
    ui_ao_args_t *a = (ui_ao_args_t*)arg;
    ui_settings_set_always_on_tile_enabled(a->enabled, a->force_off);
    lv_mem_free(a);

    s_ao_async_pending = false;   // ✅ allow future updates
}


// Keep your existing typedef + async callback:
// typedef struct { bool enabled; bool force_off; } ui_ao_args_t;
// static void ui_set_always_on_tile_async(void *arg) { ... }


static void ui_set_always_on_tile(bool enabled, bool force_off)
{
    // Edge-trigger: only act on change
    if (s_ao_wanted_enabled == enabled && s_ao_wanted_force_off == force_off) {
        return;
    }

    s_ao_wanted_enabled   = enabled;
    s_ao_wanted_force_off = force_off;

    // If LVGL is paused, DO NOT allocate or queue async work.
    // Defer until wake (we'll flush it in screen_wake_local()).
    if (s_lvgl_paused) {
        s_ao_async_pending = true;
        return;
    }

    // If one is already pending, don't enqueue another.
    if (s_ao_async_pending) {
        return;
    }

    ui_ao_args_t *a = lv_mem_alloc(sizeof(*a));
    if (!a) return;

    a->enabled   = enabled;
    a->force_off = force_off;

    s_ao_async_pending = true;
    lv_async_call(ui_set_always_on_tile_async, a);
}


static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* ---------- hooks for pausing/restarting network timers ---------- */
/* If you already have these functions, define them and remove these weak stubs. */
__attribute__((weak)) void watch_time_updates_pause(void)    {}
__attribute__((weak)) void watch_time_updates_resume(void)   {}
__attribute__((weak)) void watch_weather_updates_pause(void) {}
__attribute__((weak)) void watch_weather_updates_resume(void){}

/* ---------- Battery-aware policy clamp layer ---------- */
typedef struct {
    uint32_t screen_timeout_ms;   // 0 => never
    uint32_t wifi_off_delay_ms;
    uint32_t ble_slow_delay_ms;

    bool     force_wifi_off;      // immediate
    bool     force_screen_off;    // immediate
    bool     forbid_radio_restore;// if true, do not restore wifi on wake
} power_policy_t;

static power_policy_t policy_compute(uint32_t idle_ms)
{
    power_policy_t p = {
        .screen_timeout_ms   = screen_timeout_get_effective_ms(),
        .wifi_off_delay_ms   = g_wifi_off_delay_ms,
        .ble_slow_delay_ms   = g_ble_slow_delay_ms,
        .force_wifi_off      = false,
        .force_screen_off    = false,
        .forbid_radio_restore= false,
    };

    const shdn_state_t st = watch_shutdown_state();
    const bool latched = watch_shutdown_low_power_latched();

    // Treat latched as “critical-ish behavior” until shutdown module clears it.
    const bool criticalish = (st == SHDN_CRITICAL || st == SHDN_SHUTTING_DOWN || latched);

    if (st == SHDN_LOW) {
        // Clamp aggressive but not insane
        if (p.screen_timeout_ms == 0 || p.screen_timeout_ms > 15000) p.screen_timeout_ms = 15000;
        if (p.wifi_off_delay_ms > 30000) p.wifi_off_delay_ms = 30000;
        if (p.ble_slow_delay_ms > 20000) p.ble_slow_delay_ms = 20000;
        ui_set_always_on_tile(false, true);   // disable + force off

    }

    if (criticalish) {
        // Keep current spikes down
        p.force_wifi_off = true;
        p.forbid_radio_restore = true;

        // Get screen off quickly once idle
        // (If you want immediate screen-off in critical, set this to true unconditionally.)
        p.force_screen_off = (idle_ms >= 3000);
        ui_set_always_on_tile(false, true);   // disable + force off

        // Hard clamps (so should_screen_off triggers quickly)
        p.screen_timeout_ms = 3000;
        p.wifi_off_delay_ms = 0;
        p.ble_slow_delay_ms = 0;
    }
    if (!criticalish && st != SHDN_LOW && !latched) {
        // re-enable tile if previously disabled
        ui_set_always_on_tile(true, false);
    }
    return p;
}

/* ---------- Perf state transitions (NO RADIO CHANGES HERE) ---------- */
static void apply_perf_state(bool want_awake)
{
    if (s_perf_awake == want_awake) return;
    s_perf_awake = want_awake;

    if (want_awake) {
        (void)watch_power_set_profile_awake();
        ble_request_awake_params();
        ESP_LOGI(TAG, "Perf -> AWAKE");
    } else {
        (void)watch_power_set_profile_sleep();
        ble_request_sleep_params();
        ESP_LOGI(TAG, "Perf -> SLEEP");
    }
}

static void sleep_task(void *arg)
{
    (void)arg;
    for (;;) {
        // wait for either: timeout tick OR touch IRQ notify
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(g_screen_awake ? 50 : 300));
        manager_cb(NULL);
    }
}

/* ---------- Screen OFF (safe LVGL/TE/panel/backlight order) ---------- */
static void screen_sleep_local(void)
{
    if (!g_screen_awake) return;

    // 1) Before pausing LVGL, draw a known "blank" frame and flush once.
    if (bsp_display_lock(50)) {
        ui_show(UI_BLANK);
        lv_refr_now(NULL);
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "sleep: bsp_display_lock FAILED (blank frame skipped)");
    }

    // 2) Pause LVGL task + tick first (prevents new flushes/input reads)
    if (!s_lvgl_paused) {
        (void)lvgl_port_pause();
        s_lvgl_paused = true;
    }

    // 3) Pause TE (and gate sync) so draw_wait_cb cannot block
    bsp_display_te_pause();

    // 4) Turn panel off
    bsp_display_panel_on(false);

    // 5) Backlight off (0% PWM, does not change user preference)
    backlight_set_screen_on(false);

    g_screen_awake = false;
    ESP_LOGI(TAG, "Screen -> OFF (lvgl paused, TE paused, panel off, backlight off)");
}

/* ---------- Screen ON (safe panel/TE/LVGL/backlight + forced refresh) ---------- */
static void screen_wake_local(void)
{
    if (g_screen_awake) return;

    // 1) Turn panel on
    // NOTE: per your comment: LEAVE FALSE; BSP function behaves like a toggle in your build.
    bsp_display_panel_on(false);

    if (s_lvgl_paused) {
    (void)lvgl_port_resume();
    s_lvgl_paused = false;
    }

    // ✅ If we deferred a tile update during sleep, push it now.
    if (s_ao_async_pending) {
        // we stored the "wanted" state already; enqueue once
        ui_ao_args_t *a = lv_mem_alloc(sizeof(*a));
        if (a) {
            a->enabled   = s_ao_wanted_enabled;
            a->force_off = s_ao_wanted_force_off;
            lv_async_call(ui_set_always_on_tile_async, a);
            // keep s_ao_async_pending true until callback runs
        } else {
            // If alloc fails, leave pending true; we'll try again next wake tick.
        }
    }

    // 3) Resume TE (ungate sync)
    bsp_display_te_resume();

    // 4) Backlight on (effective brightness via backlight manager)
    backlight_set_screen_on(true);

    // 5) Show your normal UI and FORCE one refresh under lock
    if (bsp_display_lock(50)) {
        ui_show(UI_CLOCK);
        lv_refr_now(NULL);
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "wake: bsp_display_lock FAILED");
    }

    g_screen_awake = true;
    weather_request_update();
    // Optional async refreshes
    ui_notif_refresh_async();

    ESP_LOGI(TAG, "Screen -> ON (panel toggled, TE resume, lvgl resume, backlight on, forced refresh)");
}

/* ---------- Radio restore helpers (only restore what *we* forced off) ---------- */
static void maybe_restore_wifi(bool allow_restore)
{
    if (!s_wifi_forced_off) return;
    if (!allow_restore) return;

    s_wifi_forced_off = false;

    // Resume timers first (they may schedule work once Wi-Fi is up)
    watch_time_updates_resume();
    watch_weather_updates_resume();

    if (!g_wifi_on) return;
    if (g_wifi_ssid[0] == '\0') return;

    wifi_ensure_started();
    (void)wifi_start_sta(g_wifi_ssid, g_wifi_pass);
    ESP_LOGI(TAG, "WiFi -> restore (after forced-off)");
}

/* ---------- Radio stage transitions (NO SCREEN TOGGLES HERE) ---------- */
static void enter_radio_stage(sleep_stage_t st, bool allow_restore)
{
    if (s_radio_stage == st) return;

    ESP_LOGI(TAG, "Radio stage %d -> %d", (int)s_radio_stage, (int)st);
    s_radio_stage = st;

    switch (st) {
        case SLP_AWAKE:
            // Restore radios only on true user wake/interaction (screen awake).
            if (g_screen_awake) {
                maybe_restore_wifi(allow_restore);
                ble_request_awake_params();
            } else {
                // screen off: do NOT bounce perf/radio; keep BLE in sleep params
                ble_request_sleep_params();
            }
            break;

        case SLP_WIFI_OFF:
            if (g_wifi_on) {
                // Stop any periodic network pollers that keep waking stacks
                watch_time_updates_pause();
                watch_weather_updates_pause();

                s_wifi_forced_off = true;
                wifi_stop();
                ESP_LOGI(TAG, "WiFi -> OFF (forced by idle/policy), timers paused");
            }
            break;

        case SLP_BLE_SLOW:
            if (ble_is_enabled()) {
                // Low-power connection params; still connected for push notifs.
                ble_request_sleep_params();
                ESP_LOGI(TAG, "BLE -> SLOW params");
            }
            break;

        case SLP_SCREEN_OFF:
        default:
            break;
    }

    // Keep legacy global in sync (if other code reads/logs it)
    g_sleep_stage = s_radio_stage;
}

/* ---------- Touch service (IRQ-driven when screen is off) ---------- */
static void service_touch(uint32_t tnow, bool allow_restore)
{
    if (!s_tp) return;

    const bool had_irq = s_touch_pending;
    if (had_irq) {
        s_touch_pending = false;
        g_last_activity_ms = tnow;   // treat IRQ as activity
    }

    if (!g_screen_awake && had_irq) {
        // wake sequence
        s_screen_forced_off_by_timeout = false;

        screen_wake_local();

        // perf follows screen (awake now)
        apply_perf_state(true);

        // radio stage back to awake (restores forced-off radios unless policy forbids)
        enter_radio_stage(SLP_AWAKE, allow_restore);

        g_ignore_until_ms = tnow + 250;
        g_require_release_after_wake = true;
    }
}

/* ---------- Deep sleep (optional) ---------- */
#ifdef WATCH_SLEEP_ENABLE_DEEP
static void maybe_enter_deep_sleep(uint32_t idle_ms)
{
    if (idle_ms < g_deep_sleep_delay_ms) return;
    if (g_screen_awake) return;

    // Recommend only deep sleeping once radios are already down by policy.
    if (!s_wifi_forced_off) return;

    if (ble_is_enabled()) {
        ble_stop(); // explicit for deep sleep only
    }

    ESP_LOGI(TAG, "Entering deep sleep (idle=%u ms)", (unsigned)idle_ms);

    // Ensure screen is fully off (idempotent)
    screen_sleep_local();
    apply_perf_state(false);

    // TODO: configure wake sources before enabling this feature.
    esp_deep_sleep_start();
}
#endif

/* ---------- Manager tick ---------- */
static void manager_cb(lv_timer_t *t)
{
    (void)t;

    const uint32_t tnow = now_ms();
    const uint32_t idle = tnow - g_last_activity_ms;

    // Battery-aware clamps
    power_policy_t pol = policy_compute(idle);

    const bool allow_restore = !pol.forbid_radio_restore;

    /* 1) Screen policy ALWAYS applies (clamped/forced by policy) */
    const uint32_t to = pol.screen_timeout_ms;
    bool should_screen_off = (to != 0 && idle >= to);
    if (pol.force_screen_off) should_screen_off = true;

    if (should_screen_off) {
        s_screen_forced_off_by_timeout = true;
        screen_sleep_local();
    } else if (!s_screen_forced_off_by_timeout) {
        screen_wake_local();
    }


    /* 2) Perf follows screen state ONLY */
    apply_perf_state(g_screen_awake);

    /* 3) Radio staging independent of screen (clamped/forced by policy) */
    sleep_stage_t desired_radio;

    if (pol.force_wifi_off) {
        desired_radio = SLP_WIFI_OFF;
    } else if (pol.wifi_off_delay_ms == 0) {
        desired_radio = SLP_WIFI_OFF;
    } else if (idle >= pol.wifi_off_delay_ms) {
        desired_radio = SLP_WIFI_OFF;
    } else if (pol.ble_slow_delay_ms != 0 && idle >= pol.ble_slow_delay_ms) {
        desired_radio = SLP_BLE_SLOW;
    } else {
        desired_radio = SLP_AWAKE;
    }

    // NOTE: this was previously broken by an unconditional overwrite to SLP_AWAKE.
    enter_radio_stage(desired_radio, allow_restore);

    /* 4) Touch IRQ may wake us (restore may be forbidden by policy) */
    service_touch(tnow, allow_restore);

#ifdef WATCH_SLEEP_ENABLE_DEEP
    /* 5) Deep sleep last (optional) */
    maybe_enter_deep_sleep(idle);
#endif
}

/* ---------- Public API ---------- */
esp_err_t watch_sleep_init(void)
{
    g_last_activity_ms = now_ms();

    // Use the BSP touch handle (created in display init)
    s_tp = bsp_display_get_touch();
    if (!s_tp) {
        ESP_LOGW(TAG, "Touch handle NULL; wake will not work via touch");
    }

    // Initialize “last applied” states to match current globals
    s_perf_awake  = g_screen_awake ? true : false;
    s_radio_stage = SLP_AWAKE;
    g_sleep_stage = s_radio_stage;
    s_lvgl_paused = false;

    if (!s_sleep_task) {
        xTaskCreate(sleep_task, "sleep_mgr", 4096, NULL, 5, &s_sleep_task);
    }

    ESP_LOGI(TAG, "Sleep manager init OK");
    return ESP_OK;
}

void IRAM_ATTR watch_sleep_touch_irq_hint_from_isr(void)
{
    s_touch_pending = true;
    s_touch_irq_count++;

    BaseType_t hp = pdFALSE;
    if (s_sleep_task) vTaskNotifyGiveFromISR(s_sleep_task, &hp);
    if (hp) portYIELD_FROM_ISR();
}

void watch_sleep_notify_activity(void)
{
    const uint32_t tnow = now_ms();
    g_last_activity_ms = tnow;

    s_screen_forced_off_by_timeout = false;

    // Battery-aware: do not restore radios during CRITICAL/latch
    const shdn_state_t st = watch_shutdown_state();
    const bool latched = watch_shutdown_low_power_latched();
    const bool criticalish = (st == SHDN_CRITICAL || st == SHDN_SHUTTING_DOWN || latched);
    const bool allow_restore = !criticalish;

    // immediate wake (screen/perf ok)
    screen_wake_local();
    apply_perf_state(true);

    // restore radios only if allowed
    if (allow_restore) {
        enter_radio_stage(SLP_AWAKE, true);
    } else {
        // Keep Wi-Fi down; BLE stays in low-power params to prevent spikes.
        enter_radio_stage(SLP_WIFI_OFF, false);
        ble_request_sleep_params();
    }

    g_ignore_until_ms = tnow + 250;
    g_require_release_after_wake = true;
}

void watch_sleep_force_awake(void)
{
    watch_sleep_notify_activity();
}

void watch_sleep_force_screen_off(void)
{
    // Force screen off immediately + apply sleep perf.
    screen_sleep_local();
    apply_perf_state(false);

    // Do NOT force radios here; idle policy / battery policy handles them.
}
