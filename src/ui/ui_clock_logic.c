// FILE: src/ui/ui_clock_logic.c
#include "ui_priv.h"
#include "esp_log.h"
#include <time.h>
#include "ui_color_pallete.h"
static const char *UI_CLK_TAG = "UI_CLK";

extern float g_watch_batt_v;    // from watch_fuel.c
static int s_watch_batt_pct = -1;

int  g_phone_batt_pct = -1;
bool g_phone_batt_charging = false;

void apply_backlight_percent(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    bsp_display_brightness_set(pct);
}

void ui_update_ip_label(void)
{
    if (!ip_lbl) return;
    lv_label_set_text(ip_lbl, g_ip_str);
}

void ui_update_ip_label_async(void *arg)
{
    (void)arg;
    bsp_display_lock(0);
    ui_update_ip_label();
    bsp_display_unlock();
}

void ui_update_clock_async(void *arg)
{
    (void)arg;
    bsp_display_lock(0);
    clock_update_label_now();
    bsp_display_unlock();
}

void ui_update_wifi_icon_async(void *arg)
{
    (void)arg;
    bsp_display_lock(0);
    clock_update_wifi_icon_now();
    bsp_display_unlock();
}

void ui_update_ble_status_async(void *arg)
{
    (void)arg;
    bsp_display_lock(0);

    if (ble_status_lbl) {
        if (!g_ble_on) {
            lv_label_set_text(ble_status_lbl, "Off");
        } else if (g_ble_connected) {
            lv_label_set_text(ble_status_lbl, "Connected");
        } else {
            lv_label_set_text(ble_status_lbl, "Advertising");
        }
    }

    bsp_display_unlock();
}

void clock_update_label_now(void)
{
    if (!clock_time_lbl) return;

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    static char buf[24];

    if (use_24h_format) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    } else {
        int hour12 = tm_now.tm_hour % 12;
        if (hour12 == 0) hour12 = 12;
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d %s",
                 hour12, tm_now.tm_min, tm_now.tm_sec,
                 (tm_now.tm_hour >= 12) ? "PM" : "AM");
    }

    lv_label_set_text(clock_time_lbl, buf);

    if (clock_date_lbl) {
        static char dbuf[40];
        strftime(dbuf, sizeof(dbuf), "%A, %B %d", &tm_now);
        lv_label_set_text(clock_date_lbl, dbuf);
    }
}

void clock_update_wifi_icon_now(void)
{
    if (!clock_wifi_icon) return;

    if (!g_wifi_connected) {
        lv_label_set_text(clock_wifi_icon, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_opa(clock_wifi_icon, LV_OPA_50, 0);
        return;
    }

    int rssi = -100;
    if (wifi_get_rssi_dbm(&rssi) != 0) {
        lv_label_set_text(clock_wifi_icon, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_opa(clock_wifi_icon, LV_OPA_COVER, 0);
        return;
    }

    // keep single icon for now (as you had)
    lv_label_set_text(clock_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_opa(clock_wifi_icon, LV_OPA_COVER, 0);
}

void clock_update_ble_icon_now(void)
{
    if (!clock_ble_icon) return;

    if (!g_ble_on) {
        lv_label_set_text(clock_ble_icon, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_opa(clock_ble_icon, LV_OPA_30, 0);
        return;
    }

    if (!g_ble_connected) {
        lv_label_set_text(clock_ble_icon, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_opa(clock_ble_icon, LV_OPA_60, 0);
        return;
    }

    lv_label_set_text(clock_ble_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_opa(clock_ble_icon, LV_OPA_COVER, 0);
}

void ui_update_ble_icon_async(void *arg)
{
    (void)arg;
    bsp_display_lock(0);
    clock_update_ble_icon_now();
    bsp_display_unlock();
}

/* If your BLE task sets a dirty flag, keep using it */
void ui_ble_pump_updates(void)
{
    // you had ble_ui_take_dirty(); keep call if it exists
    extern bool ble_ui_take_dirty(void);
    if (!ble_ui_take_dirty()) return;

    lv_async_call(ui_update_ble_icon_async, NULL);
    lv_async_call(ui_update_ble_status_async, NULL);
}

/* ---- phone battery ---- */
static void ui_update_phone_batt_cb(void *arg)
{
    (void)arg;

    if (!phone_batt_lbl) return;

    if (g_phone_batt_pct < 0 || g_phone_batt_pct > 100) {
        lv_label_set_text(phone_batt_lbl, "");
        lv_obj_add_flag(phone_batt_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(phone_batt_lbl, LV_OBJ_FLAG_HIDDEN);

    static char buf[32];
    const char *icon = g_phone_batt_charging ? LV_SYMBOL_CHARGE : LV_SYMBOL_BATTERY_FULL;
    snprintf(buf, sizeof(buf), "%s %d%%", icon, g_phone_batt_pct);
    lv_label_set_text(phone_batt_lbl, buf);
    ESP_LOGI(UI_CLK_TAG, "Phone Batt Updated %d%%", g_phone_batt_pct);
}

void ui_set_phone_batt(int pct, bool charging)
{
    g_phone_batt_pct = pct;
    g_phone_batt_charging = charging;
    lv_async_call(ui_update_phone_batt_cb, NULL);
    ESP_LOGI(UI_CLK_TAG, "Phone Batt Set %d%% charging=%d", g_phone_batt_pct, (int)g_phone_batt_charging);
}

/* ---- watch battery ---- */
static void ui_update_watch_batt_cb(void *arg)
{
    (void)arg;
    if (!watch_batt_lbl) return;

    if (s_watch_batt_pct < 0 || s_watch_batt_pct > 100 || g_watch_batt_v <= 0.0f) {
        lv_label_set_text(watch_batt_lbl, "--%%");
        return;
    }

    lv_obj_clear_flag(watch_batt_lbl, LV_OBJ_FLAG_HIDDEN);

    static char buf[40];
    snprintf(buf, sizeof(buf), "%s %d%% %.2fV", LV_SYMBOL_BATTERY_FULL, s_watch_batt_pct, g_watch_batt_v);
    lv_label_set_text(watch_batt_lbl, buf);

    lv_obj_set_style_text_color(watch_batt_lbl, UI_COLOR(battery_color(s_watch_batt_pct)), 0);

}

void ui_set_watch_batt(int pct, float volts_unused)
{
    (void)volts_unused;
    s_watch_batt_pct = pct;
    lv_async_call(ui_update_watch_batt_cb, NULL);
}

void ui_set_watch_batt_async(void *arg)
{
    int pct = (int)(intptr_t)arg;
    ui_set_watch_batt(pct, g_watch_batt_v);
}

/* --------- clock timer ---------- */
static void clock_timer_cb_impl(lv_timer_t *t)
{
    (void)t;
    if (!g_screen_awake) return;

    clock_update_label_now();
    ui_ble_pump_updates();

    static int tick = 0;
    tick++;
    if ((tick % 5) == 0) {
        clock_update_wifi_icon_now();
        clock_update_ble_icon_now();
    }

    if (g_notif_dirty) {
        ui_notif_refresh_async();
        g_notif_dirty = false;
    }
}

// exported symbol used by your builder
void clock_timer_cb(lv_timer_t *t) { clock_timer_cb_impl(t); }

