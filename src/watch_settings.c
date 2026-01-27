#include "watch_settings.h"
#include "watch_globals.h"
#include <math.h>
#include "watch_heartrate.h"   // hr_set_boot_bpm_current()
#include "watch_screen_timeout.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_err.h"

// NEW
#include "watch_shutdown.h"

const char *SET_TAG = "GLOBALS";

// NEW: gate commits when battery is critical+
static inline bool nvs_safe_to_commit(void)
{
    shdn_state_t st = watch_shutdown_state();
    return (st == SHDN_OK || st == SHDN_LOW);
}

void settings_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(SET_TAG, "NVS needs erase (%s). Erasing...", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    if (err != ESP_OK) {
        ESP_LOGE(SET_TAG, "NVS init failed: %s", esp_err_to_name(err));
        g_nvs_ok = false;
        return;
    }

    err = nvs_open(NVS_NS, NVS_READWRITE, &g_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(SET_TAG, "NVS open failed: %s", esp_err_to_name(err));
        g_nvs_ok = false;
        return;
    }

    g_nvs_ok = true;
    ESP_LOGI(SET_TAG, "NVS ready (ns=%s)", NVS_NS);
}

void settings_load_wifi_creds(void)
{
    if (!g_nvs_ok) return;

    size_t ssid_len = sizeof(g_wifi_ssid);
    size_t pass_len = sizeof(g_wifi_pass);

    esp_err_t e1 = nvs_get_str(g_nvs, KEY_WIFI_SSID, g_wifi_ssid, &ssid_len);
    esp_err_t e2 = nvs_get_str(g_nvs, KEY_WIFI_PASS, g_wifi_pass, &pass_len);

    if (e1 != ESP_OK) g_wifi_ssid[0] = '\0';
    if (e2 != ESP_OK) g_wifi_pass[0] = '\0';
}

void settings_save_wifi_creds(const char *ssid, const char *pass)
{
    if (!g_nvs_ok) return;
    if (!ssid) ssid = "";
    if (!pass) pass = "";

    esp_err_t e1 = nvs_set_str(g_nvs, KEY_WIFI_SSID, ssid);
    esp_err_t e2 = nvs_set_str(g_nvs, KEY_WIFI_PASS, pass);

    if (e1 != ESP_OK || e2 != ESP_OK) {
        ESP_LOGE(SET_TAG, "Save wifi creds failed: %s / %s",
                 esp_err_to_name(e1), esp_err_to_name(e2));
        return;
    }

    g_settings_dirty.wifi_creds_dirty = true;

    if (nvs_safe_to_commit()) {
        esp_err_t ec = nvs_commit(g_nvs);
        ESP_LOGI(SET_TAG, "Saved wifi creds (%s)", esp_err_to_name(ec));
        if (ec == ESP_OK) g_settings_dirty.wifi_creds_dirty = false;
    } else {
        ESP_LOGW(SET_TAG, "Wi-Fi creds commit deferred (battery critical)");
    }

    snprintf(g_wifi_ssid, sizeof(g_wifi_ssid), "%s", ssid);
    snprintf(g_wifi_pass, sizeof(g_wifi_pass), "%s", pass);
}

void settings_load_from_nvs(void)
{
    if (!g_nvs_ok) return;

    int32_t b = 0;
    uint8_t u24 = 0;

    esp_err_t err_b = nvs_get_i32(g_nvs, KEY_BRIGHTNESS, &b);
    if (err_b == ESP_OK) {
        g_brightness = (int)b;
        if (g_brightness < 0) g_brightness = 0;
        if (g_brightness > 100) g_brightness = 100;
        ESP_LOGI(SET_TAG, "Loaded brightness=%d", g_brightness);
    } else {
        ESP_LOGW(SET_TAG, "Brightness not found (%s). Using default=%d",
                 esp_err_to_name(err_b), g_brightness);
    }

    esp_err_t err_24 = nvs_get_u8(g_nvs, KEY_USE_24H, &u24);
    if (err_24 == ESP_OK) {
        use_24h_format = (u24 != 0);
        ESP_LOGI(SET_TAG, "Loaded use24h=%d", (int)use_24h_format);
    } else {
        ESP_LOGW(SET_TAG, "use24h not found (%s). Using default=%d",
                 esp_err_to_name(err_24), (int)use_24h_format);
    }

    uint8_t won = 0;
    esp_err_t err_w = nvs_get_u8(g_nvs, KEY_WIFI_ON, &won);
    if (err_w == ESP_OK) {
        g_wifi_on = (won != 0);
        ESP_LOGI(SET_TAG, "Loaded wifi_on=%d", (int)g_wifi_on);
    } else {
        ESP_LOGW(SET_TAG, "wifi_on not found (%s). Using default=%d",
                 esp_err_to_name(err_w), (int)g_wifi_on);
    }

    settings_load_wifi_creds();

    uint32_t sto = 15;
    esp_err_t err_sto = nvs_get_u32(g_nvs, KEY_SCREEN_TIMEOUT, &sto);
    if (err_sto == ESP_OK) {
        if (sto != 15 && sto != 30 && sto != 60) sto = 15;
        g_screen_timeout_ms = sto * 1000;

        screen_timeout_set_default_ms(g_screen_timeout_ms);

        ESP_LOGI(SET_TAG, "Loaded screen_timeout=%us", (unsigned)sto);
    } else {
        ESP_LOGW(SET_TAG, "screen_timeout not found (%s). Using default=%ums",
                 esp_err_to_name(err_sto), (unsigned)g_screen_timeout_ms);
        }
    uint8_t bon = 0;
    esp_err_t err_ble = nvs_get_u8(g_nvs, KEY_BLE_ON, &bon);
    if (err_ble == ESP_OK) {
        g_ble_on = (bon != 0);
        ESP_LOGI(SET_TAG, "Loaded ble_on=%d", (int)g_ble_on);
    } else {
        ESP_LOGW(SET_TAG, "ble_on not found (%s). Using default=%d",
                 esp_err_to_name(err_ble), (int)g_ble_on);
    }
}

void settings_commit_dirty_now(void)
{
    if (!g_nvs_ok) return;

    // avoid flash writes in CRITICAL+
    if (!nvs_safe_to_commit()) {
        ESP_LOGW(SET_TAG, "Shutdown commit skipped (battery critical)");
        return;
    }

    bool any =
        g_settings_dirty.brightness_dirty ||
        g_settings_dirty.wifi_dirty ||
        g_settings_dirty.ble_dirty ||
        g_settings_dirty.screen_timeout_dirty ||
        g_settings_dirty.use24_dirty ||
        g_settings_dirty.wifi_creds_dirty ||
        g_settings_dirty.hr_dirty;

    if (!any) return;

    esp_err_t err = nvs_commit(g_nvs);
    ESP_LOGW(SET_TAG, "Commit dirty settings: %s", esp_err_to_name(err));

    if (err == ESP_OK) {
        g_settings_dirty = (settings_dirty_t){0};
    }
}

void settings_save_screen_timeout_s(uint32_t seconds)
{
    if (!g_nvs_ok) {
        ESP_LOGW(SET_TAG, "Save screen timeout skipped (NVS not ready)");
        return;
    }

    if (seconds != 15 && seconds != 30 && seconds != 60) seconds = 15;

    esp_err_t err = nvs_set_u32(g_nvs, KEY_SCREEN_TIMEOUT, seconds);
    if (err != ESP_OK) {
        ESP_LOGE(SET_TAG, "Save screen timeout set failed: %s", esp_err_to_name(err));
        return;
    }

    g_settings_dirty.screen_timeout_dirty = true;

    if (nvs_safe_to_commit()) {
        err = nvs_commit(g_nvs);
        ESP_LOGI(SET_TAG, "Saved screen_timeout=%us (%s)", (unsigned)seconds, esp_err_to_name(err));
        if (err == ESP_OK) g_settings_dirty.screen_timeout_dirty = false;
    } else {
        ESP_LOGW(SET_TAG, "screen_timeout commit deferred (battery critical)");
    }
}

void settings_save_brightness(int bright)
{
    if (!g_nvs_ok) {
        ESP_LOGW(SET_TAG, "Save brightness skipped (NVS not ready)");
        return;
    }

    if (bright < 0) bright = 0;
    if (bright > 100) bright = 100;

    esp_err_t err = nvs_set_i32(g_nvs, KEY_BRIGHTNESS, (int32_t)bright);
    if (err != ESP_OK) {
        ESP_LOGE(SET_TAG, "Save brightness set failed: %s", esp_err_to_name(err));
        return;
    }

    g_settings_dirty.brightness_dirty = true;

    if (nvs_safe_to_commit()) {
        err = nvs_commit(g_nvs);
        ESP_LOGI(SET_TAG, "Saved brightness=%d (%s)", bright, esp_err_to_name(err));
        if (err == ESP_OK) g_settings_dirty.brightness_dirty = false;
    } else {
        ESP_LOGW(SET_TAG, "brightness commit deferred (battery critical)");
    }
}

void settings_save_24h(bool use24)
{
    if (!g_nvs_ok) {
        ESP_LOGW(SET_TAG, "Save use24h skipped (NVS not ready)");
        return;
    }

    esp_err_t err = nvs_set_u8(g_nvs, KEY_USE_24H, use24 ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(SET_TAG, "Save use24h set failed: %s", esp_err_to_name(err));
        return;
    }

    g_settings_dirty.use24_dirty = true;

    if (nvs_safe_to_commit()) {
        err = nvs_commit(g_nvs);
        ESP_LOGI(SET_TAG, "Saved use24h=%d (%s)", (int)use24, esp_err_to_name(err));
        if (err == ESP_OK) g_settings_dirty.use24_dirty = false;
    } else {
        ESP_LOGW(SET_TAG, "use24h commit deferred (battery critical)");
    }
}

void settings_save_wifi(bool on)
{
    if (!g_nvs_ok) {
        ESP_LOGW(SET_TAG, "Save wifi_on skipped (NVS not ready)");
        return;
    }

    esp_err_t err = nvs_set_u8(g_nvs, KEY_WIFI_ON, on ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(SET_TAG, "Save wifi_on set failed: %s", esp_err_to_name(err));
        return;
    }

    g_settings_dirty.wifi_dirty = true;

    if (nvs_safe_to_commit()) {
        err = nvs_commit(g_nvs);
        ESP_LOGI(SET_TAG, "Saved wifi_on=%d (%s)", (int)on, esp_err_to_name(err));
        if (err == ESP_OK) g_settings_dirty.wifi_dirty = false;
    } else {
        ESP_LOGW(SET_TAG, "wifi_on commit deferred (battery critical)");
    }
}

void settings_save_ble(bool on)
{
    if (!g_nvs_ok) {
        ESP_LOGW(SET_TAG, "Save ble_on skipped (NVS not ready)");
        return;
    }

    esp_err_t err = nvs_set_u8(g_nvs, KEY_BLE_ON, on ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(SET_TAG, "Save ble_on set failed: %s", esp_err_to_name(err));
        return;
    }

    g_settings_dirty.ble_dirty = true;

    if (nvs_safe_to_commit()) {
        err = nvs_commit(g_nvs);
        ESP_LOGI(SET_TAG, "Saved ble_on=%d (%s)", (int)on, esp_err_to_name(err));
        if (err == ESP_OK) g_settings_dirty.ble_dirty = false;
    } else {
        ESP_LOGW(SET_TAG, "BLE save deferred (battery critical)");
    }
}

void settings_save_hr_current(float bpm, bool valid)
{
    if (!g_nvs_ok) return;

    g_settings_dirty.hr_dirty = true;

    uint8_t v = valid ? 1 : 0;
    esp_err_t e1 = nvs_set_u8(g_nvs, KEY_HR_CUR_VALID, v);
    if (e1 != ESP_OK) {
        ESP_LOGE(SET_TAG, "Save HR valid failed: %s", esp_err_to_name(e1));
        return;
    }

    if (valid) {
        if (bpm < 30.0f) bpm = 30.0f;
        if (bpm > 240.0f) bpm = 240.0f;

        int32_t bpm_x100 = (int32_t)lroundf(bpm * 100.0f);
        esp_err_t e2 = nvs_set_i32(g_nvs, KEY_HR_CUR_BPM_X100, bpm_x100);
        if (e2 != ESP_OK) {
            ESP_LOGE(SET_TAG, "Save HR bpm failed: %s", esp_err_to_name(e2));
            return;
        }
    }

    if (nvs_safe_to_commit()) {
        esp_err_t ec = nvs_commit(g_nvs);
        if (ec == ESP_OK) g_settings_dirty.hr_dirty = false;
    }
}

void settings_load_hr_current_into_ui(void)
{
    if (!g_nvs_ok) return;

    uint8_t v = 0;
    int32_t bpm_x100 = 0;

    esp_err_t ev = nvs_get_u8(g_nvs, KEY_HR_CUR_VALID, &v);
    esp_err_t eb = nvs_get_i32(g_nvs, KEY_HR_CUR_BPM_X100, &bpm_x100);

    bool valid = (ev == ESP_OK) && (v != 0) && (eb == ESP_OK);
    float bpm = valid ? ((float)bpm_x100 / 100.0f) : 0.0f;

    hr_set_boot_bpm_current(bpm, valid);
}
