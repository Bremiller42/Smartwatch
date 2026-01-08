#include "watch_power.h"
#include "esp_pm.h"
#include "esp_log.h"

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
