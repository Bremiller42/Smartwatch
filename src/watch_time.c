#include "watch_time.h"
#include "watch_globals.h"
#include "watch_ui.h"     // for ui_update_clock_async
#include "watch_audio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "watch_i2c.h"
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>

/* ---------------- TM_TAG ---------------- */
const char *TM_TAG = "TIME";


static void time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    g_time_synced = true;
    ESP_LOGI(TM_TAG, "SNTP time synced");
    watch_audio_beep(880, 70);
    watch_audio_beep(1175, 90);

    time_save_last_known();
    lv_async_call(ui_update_clock_async, NULL);
}

void time_set_timezone(void)
{
    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
    tzset();
}

void time_save_last_known(void)
{
    if (!g_nvs_ok) return;

    time_t now = time(NULL);
    int64_t us = esp_timer_get_time();

    nvs_set_i64(g_nvs, KEY_LAST_EPOCH, (int64_t)now);
    nvs_set_i64(g_nvs, KEY_LAST_US, us);
    nvs_commit(g_nvs);
}

void time_restore_last_known(void)
{
    if (!g_nvs_ok) return;

    int64_t saved_epoch = 0;
    int64_t saved_us = 0;

    if (nvs_get_i64(g_nvs, KEY_LAST_EPOCH, &saved_epoch) != ESP_OK) return;
    if (nvs_get_i64(g_nvs, KEY_LAST_US, &saved_us) != ESP_OK) return;

    int64_t now_us = esp_timer_get_time();
    int64_t delta_us = now_us - saved_us;
    if (delta_us < 0) delta_us = 0;

    time_t restored = (time_t)(saved_epoch + (delta_us / 1000000LL));

    struct timeval tv = { .tv_sec = restored, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    ESP_LOGI(TM_TAG, "Restored time from NVS: epoch=%lld (+%llds)",
             (long long)saved_epoch, (long long)(delta_us / 1000000LL));
}

void sntp_start(void)
{
    if (esp_sntp_enabled()) {
        ESP_LOGI(TM_TAG, "SNTP already running");
        return;
    }

    time_set_timezone();

    ESP_LOGI(TM_TAG, "Starting SNTP...");
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);

    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.nist.gov");

    esp_sntp_set_sync_interval(60 * 60 * 1000);
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
}

void set_system_time_hm(int hour, int minute)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    tm_now.tm_hour = hour;
    tm_now.tm_min  = minute;
    tm_now.tm_sec  = 0;

    time_t new_time = mktime(&tm_now);

    struct timeval tv = { .tv_sec = new_time, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}
