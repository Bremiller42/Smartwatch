#include "esp_pm.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_sleep.h"   // <-- needed for esp_deep_sleep_start()

#include "watch_power.h"
#include "watch_settings.h"
#include "watch_wifi.h"
#include "watch_ble.h"

static const char *TAG = "PWR";

esp_err_t watch_power_init(void)
{
#if !CONFIG_PM_ENABLE
    ESP_LOGW(TAG, "CONFIG_PM_ENABLE is OFF. Power management not enabled in sdkconfig.");
    return ESP_OK;
#else
    esp_pm_config_t pm = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 40,
        .light_sleep_enable = true,
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err == ESP_OK) ESP_LOGI(TAG, "PM configured (auto light-sleep enabled).");
    else ESP_LOGE(TAG, "esp_pm_configure failed: %s", esp_err_to_name(err));
    return err;
#endif
}

esp_err_t watch_power_set_profile_awake(void)
{
#if CONFIG_PM_ENABLE
    ESP_LOGI(TAG, "Setting AWAKE power profile");
    esp_pm_config_t pm = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 80,
        .light_sleep_enable = true,
    };
    return esp_pm_configure(&pm);
#else
    return ESP_OK;
#endif
}

esp_err_t watch_power_set_profile_sleep(void)
{
#if CONFIG_PM_ENABLE
    ESP_LOGI(TAG, "Setting SLEEP power profile");
    // Aggressive but safe sleep profile:
    esp_pm_config_t pm = {
        .max_freq_mhz = 100,
        .min_freq_mhz = 40,
        .light_sleep_enable = true,
    };
    return esp_pm_configure(&pm);
#else
    return ESP_OK;
#endif
}

static void shutdown_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "User requested shutdown");

    // Best-effort save
    settings_commit_dirty_now();

    // Turn off big hitters
    wifi_stop();
    ble_set_enabled(false);

    // You might want to set wake sources here (touch, RTC, etc.)
    // esp_sleep_enable_ext0_wakeup(GPIO_NUM_X, 0);

    esp_deep_sleep_start();
}

void watch_power_shutdown_async(void)
{
    // runs shutdown_cb from esp_timer task, not inside LVGL handler
    const esp_timer_create_args_t targs = {
        .callback = &shutdown_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "pwr_shdn",
        .skip_unhandled_events = true,
    };

    esp_timer_handle_t t;
    if (esp_timer_create(&targs, &t) == ESP_OK) {
        (void)esp_timer_start_once(t, 50 * 1000); // 50ms
    } else {
        // Worst case: do it immediately
        shutdown_cb(NULL);
    }
}
static void restart_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "User requested restart (graceful)");

    // Best-effort save (commit once)
    settings_commit_dirty_now();

    // Stop big hitters to avoid tearing down mid-flight
    wifi_stop();
    ble_set_enabled(false);

    // Optional: tiny delay so logs flush / tasks settle
    // (esp_timer task context; keep short)
    // vTaskDelay isn't available here unless you include FreeRTOS;
    // instead just restart immediately or schedule a second timer.

    esp_restart();
}

void watch_power_restart_async(void)
{
    const esp_timer_create_args_t targs = {
        .callback = &restart_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "pwr_rst",
        .skip_unhandled_events = true,
    };

    esp_timer_handle_t t;
    if (esp_timer_create(&targs, &t) == ESP_OK) {
        (void)esp_timer_start_once(t, 80 * 1000); // ~80ms after UI blank loads
    } else {
        restart_cb(NULL);
    }
}