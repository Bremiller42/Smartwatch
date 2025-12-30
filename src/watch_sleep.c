#include "watch_sleep.h"
#include "watch_globals.h"
#include "watch_ui.h"

#include "display.h"
#include "esp_bsp.h"
#include "lv_port.h"

#include "esp_log.h"

/* ---------------- SLP_TAG ---------------- */
const char *SLP_TAG = "SLEEP";


/* LVGL internal read (your project used this symbol) */
void _lv_indev_read(lv_indev_t * indev, lv_indev_data_t * data);

static void screen_sleep(void);
static void screen_wake(void);

static void sleep_timer_cb(lv_timer_t *t);
static void touch_activity_timer_cb(lv_timer_t *t);

static void wake_blocker_event_cb(lv_event_t *e);
static void wake_blocker_create(void);
static void wake_blocker_remove(void);

static void screen_sleep(void)
{
    if (!g_screen_awake) return;
    g_screen_awake = false;
    
    ui_show(UI_BLANK);
    
    ESP_LOGI(SLP_TAG, "Screen -> SLEEP");
    g_blocker_active = true;
    wake_blocker_create();
    ESP_LOGI(SLP_TAG, "Screen -> LOCKED");

    bsp_display_backlight_off();

    if (clock_timer) lv_timer_pause(clock_timer);
}

static void screen_wake(void)
{
    if (g_screen_awake) return;
    g_screen_awake = true;

    ESP_LOGI(SLP_TAG, "Screen -> WAKE");

    apply_backlight_percent(g_brightness);

    g_last_activity_ms = lv_tick_get();

    g_ignore_until_ms = lv_tick_get() + WAKE_DEBOUNCE_MS;
    g_blocker_active = true;

    g_require_release_after_wake = true;

    wake_blocker_create();

    if (clock_timer) lv_timer_resume(clock_timer);
    ui_show(UI_CLOCK);

}

static void wake_blocker_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (!g_screen_awake) {
        if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING || code == LV_EVENT_CLICKED) {
            mark_user_activity();
        }
        lv_event_stop_bubbling(e);
        lv_event_stop_processing(e);
        return;
    }

    if (g_blocker_active) {
        lv_event_stop_bubbling(e);
        lv_event_stop_processing(e);
    }
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

void mark_user_activity(void)
{
    g_last_activity_ms = lv_tick_get();

    if (!g_screen_awake) {
        screen_wake();
    }
}

static void sleep_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_screen_awake) return;

    uint32_t now = lv_tick_get();
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

    uint32_t now = lv_tick_get();

    if (!g_screen_awake) {
        if (data.state == LV_INDEV_STATE_PRESSED) {
            mark_user_activity();
        }
        return;
    }

    if ((int32_t)(now - g_ignore_until_ms) < 0) {
        g_blocker_active = true;
        return;
    }

    if (g_require_release_after_wake) {
        g_blocker_active = true;
        if (data.state == LV_INDEV_STATE_RELEASED) {
            g_require_release_after_wake = false;
        }
        return;
    }

    g_blocker_active = false;
    wake_blocker_remove();

    if (data.state == LV_INDEV_STATE_PRESSED) {
        mark_user_activity();
    }
}

void activity_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED ||
        code == LV_EVENT_PRESSING ||
        code == LV_EVENT_CLICKED ||
        code == LV_EVENT_GESTURE)
    {
        mark_user_activity();
    }
}

void sleep_system_init(void)
{
    g_last_activity_ms = lv_tick_get();
    g_screen_awake = true;

    if (!sleep_timer) {
        sleep_timer = lv_timer_create(sleep_timer_cb, 250, NULL);
    }
    if (!touch_activity_timer) {
        touch_activity_timer = lv_timer_create(touch_activity_timer_cb, 20, NULL);
    }
}
