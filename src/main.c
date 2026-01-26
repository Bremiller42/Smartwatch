// FILE: main.c

#include <inttypes.h>
#include <stdio.h>

#include "esp_bsp.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "freertos/task.h"

#include "display.h"
#include "lv_port.h"

#include "watch_audio.h"
#include "watch_ble.h"
#include "watch_fuel.h"
#include "watch_globals.h"
#include "watch_heartrate.h"
#include "watch_i2c.h"
#include "watch_settings.h"
#include "watch_sleep.h"
#include "watch_time.h"
#include "ui_priv.h"
#include "watch_wifi.h"
#include "watch_power.h"
#include "watch_logbuf.h"
#include "watch_loghook.h"
#include "watch_logstream.h"
#include "watch_shutdown.h"

static const char *MAIN_TAG = "SmartWatch";

#define LOG_SECTION(section) \
    ESP_LOGI(MAIN_TAG, "\n\n************* %s **************\n", (section))

static void init_logs_and_chipinfo(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);

    ESP_LOGI(MAIN_TAG, "Chip: %s, cores: %d, features: %s%s%s%s",
             CONFIG_IDF_TARGET,
             chip_info.cores,
             (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
             (chip_info.features & CHIP_FEATURE_BT) ? "BT/" : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? "BLE/" : "",
             (chip_info.features & CHIP_FEATURE_IEEE802154) ? "802.15.4" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    ESP_LOGI(MAIN_TAG, "Silicon revision v%d.%d", major_rev, minor_rev);

    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(MAIN_TAG, "%" PRIu32 "MB flash", flash_size / (uint32_t)(1024 * 1024));
    } else {
        ESP_LOGW(MAIN_TAG, "Get flash size failed");
    }

    ESP_LOGI(MAIN_TAG, "Minimum free heap: %" PRIu32 " bytes", esp_get_minimum_free_heap_size());
    ESP_LOGI(MAIN_TAG, "Free PSRAM: %d bytes", (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void boot_guard_shutdown_cb(void *arg)
{
    (void)arg;
    watch_power_shutdown_async();
}

/* returns true if we took over the boot (show low power screen + shutdown) */
static bool boot_guard_check_and_arm(void)
{
    // If you’re using RTC latch:
    bool latched = watch_shutdown_low_power_latched();

    float vbat = -1.0f;
    esp_err_t e = watch_fuel_read_vcell(&vbat);

    float crit = watch_shutdown_get_critical_v();  // from watch_shutdown.c API we discussed

    if ((e == ESP_OK && vbat > 0.0f && vbat <= crit) || latched) {

        ESP_LOGW(MAIN_TAG, "BOOT GUARD: vbat=%.2f crit=%.2f latched=%d -> LOW_PWR then sleep",
                 vbat, crit, (int)latched);

        // keep it visible, but dim
        g_screen_awake = true;
        apply_backlight_percent(15);

        // stop radios (optional but recommended)
        wifi_stop();
        // ble_set_enabled(false);

        // show the low power screen right now
        bsp_display_lock(0);
        ui_show(UI_LOW_PWR);
        bsp_display_unlock();

        // schedule shutdown in 5 seconds
        static esp_timer_handle_t t = NULL;
        if (!t) {
            const esp_timer_create_args_t ta = {
                .callback = &boot_guard_shutdown_cb,
                .name = "boot_guard",
                .dispatch_method = ESP_TIMER_TASK,
                .skip_unhandled_events = true,
            };
            (void)esp_timer_create(&ta, &t);
        }
        if (t) {
            (void)esp_timer_stop(t);
            (void)esp_timer_start_once(t, 5 * 1000 * 1000);
        } else {
            watch_power_shutdown_async();
        }

        return true;
    }

    return false;
}

static void display_init(void)
{
    LOG_SECTION("Initialize panel device");

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size   = EXAMPLE_LCD_QSPI_H_RES * EXAMPLE_LCD_QSPI_V_RES,
#if LVGL_PORT_ROTATION_DEGREE == 90
        .rotate = LV_DISP_ROT_90,
#elif LVGL_PORT_ROTATION_DEGREE == 270
        .rotate = LV_DISP_ROT_270,
#elif LVGL_PORT_ROTATION_DEGREE == 180
        .rotate = LV_DISP_ROT_180,
#else
        .rotate = LV_DISP_ROT_NONE,
#endif
    };

    bsp_display_start_with_config(&cfg);

    // Apply backlight early (user setting)
    apply_backlight_percent(g_brightness);
}

static void ui_init(void)
{
    LOG_SECTION("Create UI");
    bsp_display_lock(0);
    create_watch_ui();
    bsp_display_unlock();
}

static void bringup_services(void)
{
    // WiFi autostart (only if enabled AND has SSID)
    if (g_wifi_on && g_wifi_ssid[0]) {
        ESP_LOGI(MAIN_TAG, "Auto WiFi enabled from NVS -> starting STA");
        wifi_start_sta(g_wifi_ssid, g_wifi_pass);
    }

    // Start sensor/service tasks AFTER I2C
    start_max30102_task();
    watch_fuel_start_task();
}

void setup(void);

#if !CONFIG_AUTOSTART_ARDUINO
void app_main(void)
{
    setup();
}
#endif

void setup(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(MAIN_TAG, ESP_LOG_INFO);
    watch_logstream_init();

    watch_audio_init();
    watch_audio_beep_async_init();

    LOG_SECTION("Smartwatch start");
    init_logs_and_chipinfo();

    LOG_SECTION("Initialize NVS settings");
    settings_nvs_init();
    settings_load_from_nvs();

    LOG_SECTION("Init power");
    watch_power_init();

    LOG_SECTION("Init display");
    display_init();

    LOG_SECTION("Create UI");
    ui_init();

    LOG_SECTION("Init sleep system");
    sleep_system_init();

    LOG_SECTION("Init I2C (early)");
    ESP_ERROR_CHECK(watch_i2c_init());

    LOG_SECTION("Init shutdown controller");
    watch_shutdown_init();

    // ✅ Optional: probe MAX17048 once (so vcell read is reliable)
    (void)watch_fuel_init();

    // ✅ BOOT GUARD: if low/latched, show UI_LOW_PWR and schedule shutdown, then STOP boot
    if (boot_guard_check_and_arm()) {
        return; // do NOT start BLE/WiFi/tasks; we’re going to deep sleep in 5s
    }

    LOG_SECTION("Initialize BLE");
    ESP_ERROR_CHECK(ble_init(NULL));
    ble_set_enabled(g_ble_on);

    LOG_SECTION("Initialize time zone");
    time_set_timezone();
    time_restore_last_known();

    esp_log_level_set("NimBLE", ESP_LOG_WARN);
    settings_load_hr_current_into_ui();

    LOG_SECTION("Bring up services");
    bringup_services();   // <-- update this to NOT call watch_i2c_init anymore

    watch_audio_beep(880,  60);
    watch_audio_beep(1320, 50);
    watch_audio_beep(1760, 70);
}


void loop(void)
{
    // No Arduino loop used (ESP-IDF tasks drive everything)
}
