#include "watch_sleep.h"

#include "display.h"
#include "lv_port.h"
#include "esp_bsp.h"

#include "esp_sleep.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#include "watch_audio.h"
#include "watch_power.h"
#include "watch_wifi.h"
#include "watch_ble.h"
#include "watch_globals.h"
#include "ui_priv.h"

/* LVGL internal read (your project used this symbol) */
void _lv_indev_read(lv_indev_t * indev, lv_indev_data_t * data);

static const char *SLP_TAG = "SLEEP";
static const char *PWR_TAG = "PWR";

uint32_t g_wifi_off_delay_ms = (2 * 60 * 1000);

static lv_timer_t *wifi_off_timer = NULL;
static uint32_t g_wifi_off_deadline_ms = 0;
static bool g_wifi_forced_off_by_sleep = false;

extern int g_brightness;

#define WAKE_DEBOUNCE_MS 250

#ifndef TOUCH_INT_GPIO
#define TOUCH_INT_GPIO 3
#endif

#define TOUCH_INT_ACTIVE_LOW 1

// Re-enable the polling timer (like your original working version)

static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static inline bool touch_int_asserted(void)
{
#if TOUCH_INT_ACTIVE_LOW
    return gpio_get_level(TOUCH_INT_GPIO) == 0;
#else
    return gpio_get_level(TOUCH_INT_GPIO) == 1;
#endif
}

static inline bool wake_debounce_done(void)
{
    return (int32_t)(now_ms() - g_ignore_until_ms) >= 0;
}

/* forward decls */
static void wake_blocker_event_cb(lv_event_t *e);
static void wake_blocker_create(void);
static void wake_blocker_remove(void);

static void screen_sleep(void);
static void screen_wake(void);

static void sleep_timer_cb(lv_timer_t *t);
static void wifi_off_timer_cb(lv_timer_t *t);
static void touch_activity_timer_cb(lv_timer_t *t);

static void arm_gpio_wakeup(void)
{
    // gpio_config_t io = {
    //     .pin_bit_mask = 1ULL << TOUCH_INT_GPIO,
    //     .mode = GPIO_MODE_INPUT,
    //     .pull_up_en = GPIO_PULLUP_ENABLE,
    //     .pull_down_en = GPIO_PULLDOWN_DISABLE,
    //     .intr_type = GPIO_INTR_DISABLE,
    // };
    // ESP_ERROR_CHECK(gpio_config(&io));

    ESP_ERROR_CHECK(gpio_wakeup_enable((gpio_num_t)TOUCH_INT_GPIO,
#if TOUCH_INT_ACTIVE_LOW
                                       GPIO_INTR_LOW_LEVEL
#else
                                       GPIO_INTR_HIGH_LEVEL
#endif
    ));
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());

    ESP_LOGI(SLP_TAG, "GPIO wake armed on pin=%d (level=%s)",
             TOUCH_INT_GPIO,
#if TOUCH_INT_ACTIVE_LOW
             "LOW"
#else
             "HIGH"
#endif
    );
}

static void light_sleep_until_touch(void)
{
    arm_gpio_wakeup();

    // If finger already down, don't sleep.
    if (touch_int_asserted()) return;

    ESP_LOGI(SLP_TAG, "entering light sleep...");
    esp_err_t err = esp_light_sleep_start();
    if (err != ESP_OK) {
        ESP_LOGW(PWR_TAG, "esp_light_sleep_start err=%s", esp_err_to_name(err));
    }
}

static void wifi_off_timer_cb(lv_timer_t *t)
{
    (void)t;

    if (g_screen_awake) return;
    if (!g_wifi_on) return;

    uint32_t now = now_ms();
    if ((int32_t)(now - g_wifi_off_deadline_ms) < 0) return;

    ESP_LOGI(PWR_TAG, "Screen off grace expired -> Wi-Fi OFF");
    wifi_stop();
    g_wifi_forced_off_by_sleep = true;

    if (wifi_off_timer) lv_timer_pause(wifi_off_timer);
}

static void screen_sleep(void)
{
    if (!g_screen_awake) return;
    g_screen_awake = false;

    ui_show(UI_BLANK);
    ESP_LOGI(SLP_TAG, "Screen -> SLEEP");

    // Swallow touches while asleep
    g_blocker_active = true;
    wake_blocker_create();

    if (sleep_timer) lv_timer_pause(sleep_timer);

    // Slow down the polling while asleep (like your original code)
    if (touch_activity_timer) lv_timer_set_period(touch_activity_timer, 250);

    // Backlight-only sleep (no LVGL/TE/panel pause)
    bsp_display_backlight_off();
    bsp_display_te_pause();

    watch_power_set_profile_sleep();
    ble_request_sleep_params();

    // Wi-Fi grace
    g_wifi_forced_off_by_sleep = false;
    if (g_wifi_on) {
        g_wifi_off_deadline_ms = now_ms() + g_wifi_off_delay_ms;
        if (!wifi_off_timer) wifi_off_timer = lv_timer_create(wifi_off_timer_cb, 1000, NULL);
        else lv_timer_resume(wifi_off_timer);
    }

    // Sleep until touch INT (or other wake reason)
    light_sleep_until_touch();

    // We woke -> run wake sequence now
    screen_wake();
}

static void screen_wake(void)
{
    if (g_screen_awake) return;
    g_screen_awake = true;

    // NOTE: If USB logging dies after light sleep, you may not see this log.
    ESP_LOGI(SLP_TAG, "Screen -> WAKE");

    watch_power_set_profile_awake();
    ble_request_awake_params();

    bsp_display_te_resume();
    // Restore backlight
    apply_backlight_percent(g_brightness);

    // Resume timers
    if (sleep_timer) lv_timer_resume(sleep_timer);
    if (wifi_off_timer) lv_timer_pause(wifi_off_timer);

    // Speed up polling while awake
    if (touch_activity_timer) lv_timer_set_period(touch_activity_timer, 20);

    // Debounce / blocker
    g_last_activity_ms = now_ms();
    g_ignore_until_ms  = now_ms() + WAKE_DEBOUNCE_MS;
    g_blocker_active = true;
    g_require_release_after_wake = true;
    wake_blocker_create();

    // Switch UI
    ui_show(UI_CLOCK);
    ui_notif_refresh_async();

    // Wi-Fi reconnect if needed
    if (g_wifi_on && !g_wifi_connected) {
        wifi_start_sta(g_wifi_ssid, g_wifi_pass);
    }
}

/* ---------------- Wake blocker ---------------- */
static void wake_blocker_event_cb(lv_event_t *e)
{
    // Keep this, but don’t rely on it to unlock anymore.
    // Unlocking will be handled by touch_activity_timer_cb (more reliable).
    lv_event_stop_bubbling(e);
    lv_event_stop_processing(e);
}

static void wake_blocker_create(void)
{
    if (wake_blocker) return;

    wake_blocker = lv_obj_create(lv_layer_top());
    lv_obj_set_size(wake_blocker, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(wake_blocker, LV_OPA_0, 0);
    lv_obj_set_style_border_opa(wake_blocker, LV_OPA_0, 0);
    lv_obj_clear_flag(wake_blocker, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_flag(wake_blocker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wake_blocker, wake_blocker_event_cb, LV_EVENT_ALL, NULL);
}

static void wake_blocker_remove(void)
{
    if (wake_blocker) {
        lv_obj_del(wake_blocker);
        wake_blocker = NULL;
        ESP_LOGI(SLP_TAG, "Screen -> UNLOCKED");
    }
}

/* ---------------- Activity ---------------- */
void mark_user_activity(void)
{
    g_last_activity_ms = now_ms();
    if (!g_screen_awake) screen_wake();
}

static void sleep_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_screen_awake) return;

    uint32_t now = now_ms();
    if ((now - g_last_activity_ms) >= g_screen_timeout_ms) {
        screen_sleep();
    }
}

static void touch_activity_timer_cb(lv_timer_t *t)
{
    (void)t;

    lv_indev_t *indev = bsp_display_get_input_dev();
    if (!indev) return;

    lv_indev_data_t data;
    _lv_indev_read(indev, &data);

    uint32_t now = now_ms();

    // If asleep: any press wakes
    if (!g_screen_awake) {
        if (data.state == LV_INDEV_STATE_PRESSED) {
            mark_user_activity(); // calls screen_wake()
        }
        return;
    }

    // If within debounce window: keep blocker active
    if ((int32_t)(now - g_ignore_until_ms) < 0) {
        g_blocker_active = true;
        return;
    }

    // Require release after wake: don’t unlock until we see RELEASED
    if (g_require_release_after_wake) {
        g_blocker_active = true;
        if (data.state == LV_INDEV_STATE_RELEASED) {
            g_require_release_after_wake = false;
        }
        return;
    }
    
    // Unlock after debounce + release
    g_blocker_active = false;
    wake_blocker_remove();

    // Normal activity tracking
    if (data.state == LV_INDEV_STATE_PRESSED) {
        mark_user_activity();
    }
}

void activity_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING ||
        code == LV_EVENT_CLICKED || code == LV_EVENT_GESTURE) {
        mark_user_activity();
    }
}

void sleep_system_init(void)
{
    g_last_activity_ms = now_ms();
    g_screen_awake = true;

    if (!sleep_timer) sleep_timer = lv_timer_create(sleep_timer_cb, 250, NULL);

    // Bring back the polling timer that makes wake-blocker reliable
    if (!touch_activity_timer) touch_activity_timer = lv_timer_create(touch_activity_timer_cb, 20, NULL);
}
