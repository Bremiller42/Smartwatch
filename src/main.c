#include <lvgl.h>

#include "display.h"
#include "esp_bsp.h"
#include "lv_port.h"
#include "watch_ble.h"


#include "watch_globals.h"
#include "watch_settings.h"
#include "watch_time.h"
#include "watch_wifi.h"
#include "watch_sleep.h"
#include "watch_ui.h"

#include <esp_log.h>
#include <esp_flash.h>
#include <esp_chip_info.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <inttypes.h>

#define logSection(section) \
  ESP_LOGI(MAIN_TAG, "\n\n************* %s **************\n", section);


/* ---------------- MAIN_TAG ---------------- */  
const char *MAIN_TAG = "SmartWatch";

  
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

    logSection("Smartwatch start");
    init_logs_and_chipinfo();

    logSection("Initialize NVS settings");
    settings_nvs_init();
    settings_load_from_nvs();

    logSection("Initialize BLE");
    ble_init(NULL);
    if (g_ble_on) {
        ble_start();
    }


    logSection("Initialize time zone");
    time_set_timezone();
    time_restore_last_known();

    logSection("Initialize panel device");

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

    apply_backlight_percent(g_brightness);

    logSection("Create UI");
    bsp_display_lock(0);
    create_watch_ui();
    bsp_display_unlock();

    logSection("Init sleep system");
    sleep_system_init();

    if (g_wifi_on && g_wifi_ssid[0]) {
        ESP_LOGI(MAIN_TAG, "Auto WiFi enabled from NVS -> starting STA");
        wifi_start_sta(g_wifi_ssid, g_wifi_pass);
    }

    logSection("Smartwatch ready");
}

void loop(void)
{
}
