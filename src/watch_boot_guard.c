// FILE: src/watch_boot_guard.c  (new)
#include "watch_boot_guard.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "watch_globals.h"
#include "watch_power.h"
#include "watch_fuel.h"
#include "ui_priv.h"

static const char *TAG = "BOOT_GUARD";

// choose your critical voltage threshold
#define VBAT_CRITICAL_BOOT   3.55f
#define LOWPWR_SHOW_MS       5000

static void lowpwr_shutdown_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "Boot guard: shutting down after low battery screen");
    watch_power_shutdown_async();   // deep sleep
}

void watch_boot_guard_check_vbat(void)
{
    float vbat = g_watch_batt_v;

    // If fuel hasn't run yet, don't false-trigger.
    // Keep it simple: only guard if we have a valid-looking value.
    if (vbat < 2.5f || vbat > 4.5f) {
        ESP_LOGW(TAG, "VBAT invalid/unset (%.2f) - skipping boot guard", vbat);
        return;
    }

    if (vbat > VBAT_CRITICAL_BOOT) {
        ESP_LOGI(TAG, "VBAT OK for boot: %.2fV", vbat);
        return;
    }

    ESP_LOGE(TAG, "VBAT LOW at boot: %.2fV <= %.2fV -> show LOW_PWR then sleep",
             vbat, (float)VBAT_CRITICAL_BOOT);

    ui_show(UI_LOW_PWR);

    const esp_timer_create_args_t targs = {
        .callback = &lowpwr_shutdown_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "boot_lowpwr",
        .skip_unhandled_events = true,
    };

    esp_timer_handle_t t = NULL;
    if (esp_timer_create(&targs, &t) == ESP_OK) {
        (void)esp_timer_start_once(t, LOWPWR_SHOW_MS * 1000ULL);
    } else {
        // worst case: shutdown immediately
        watch_power_shutdown_async();
    }
}
