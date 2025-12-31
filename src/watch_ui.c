// watch_ui.c (single-file, complete, cleaned version)
// - Settings screen: NON-scrollable TabView (Display / Network / Time&Date)
// - Tiles: rounded square tiles; switch-tiles are color-only (no visible switch)
// - Notifications: uses ONLY the new notif_bar (no old notif_chip/notif_icon_row code)
// - Safe refresh path: all UI updates funnel through lv_async_call()

#include "watch_ui.h"
#include "watch_wifi.h"
#include "watch_settings.h"
#include "watch_time.h"
#include "watch_sleep.h"
#include "watch_globals.h"
#include "watch_ble.h"
#include "watch_audio.h"

#include "display.h"
#include "esp_bsp.h"
#include "lv_port.h"
#include "fonts/orbitron_72.h"

#include "esp_log.h"

#include <stdio.h>
#include <time.h>
#include <string.h>
#include <inttypes.h>

/* ---------------- Tag ---------------- */
static const char *UI_TAG = "UI";

/* ---------- forward decls for local UI pieces ---------- */
static lv_obj_t *build_home_screen(void);
static lv_obj_t *build_clock_screen(void);
static lv_obj_t *build_settings_screen(void);
static lv_obj_t *build_black_screen(void);

static void on_back_to_home(lv_event_t * e);
static void on_open_clock(lv_event_t * e);
static void on_open_settings(lv_event_t * e);
static void on_settings_back(lv_event_t *e);

static void on_brightness_changed(lv_event_t *e);
static void on_time_format_changed(lv_event_t *e);
static void on_wifi_toggle(lv_event_t *e);

static void on_open_wifi_picker(lv_event_t *e);
void on_wifi_ap_clicked(lv_event_t *e);  // used by wifi module
static void on_wifi_picker_close(lv_event_t *e);
static void on_wifi_picker_scan(lv_event_t *e);

static void open_settime_modal(lv_obj_t *parent);
static void settime_close(void);
static void settime_refresh_labels(void);
static void on_open_settime(lv_event_t *e);
static void on_settime_cancel(lv_event_t * e);
static void on_settime_save(lv_event_t * e);
static void on_inc_hour(lv_event_t *e);
static void on_dec_hour(lv_event_t *e);
static void on_inc_min(lv_event_t *e);
static void on_dec_min(lv_event_t *e);

static void clock_timer_cb(lv_timer_t *t);

void on_wifi_pass_cancel(lv_event_t *e);
void on_wifi_pass_connect(lv_event_t *e);

static void on_ble_toggle(lv_event_t *e);
void ui_update_ble_status_async(void *arg);
void ui_update_ble_icon_async(void *arg);

/* ---------------- Misc UI state ---------------- */
static lv_obj_t *timeout_sub_lbl = NULL;

/* ---------------- Notifications (NEW BAR ONLY) ---------------- */
#define NOTIF_ICON_W 28
#define NOTIF_ICON_H 28
#define NOTIF_GAP    10

static lv_obj_t *notif_bar = NULL;                   // container (clock screen)
static lv_obj_t *notif_slot[NOTIF_MAX] = {0};        // each icon slot


static void notif_icons_refresh(void);

static void ui_notif_refresh_cb(void *arg)
{
    (void)arg;
    notif_icons_refresh();
    g_notif_dirty = false;
}

void ui_notif_refresh_async(void)
{
    // safe to call from anywhere (ble task, sleep task, etc)
    lv_async_call(ui_notif_refresh_cb, NULL);
}

/* ---------------- UI Helpers ---------------- */

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

/* ---------------- Clock ---------------- */

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

    // Map RSSI later if you want; keep single icon for now
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

void ui_ble_pump_updates(void)
{
    if (!ble_ui_take_dirty()) return;

    // Called from UI timer -> ok to schedule async updates
    lv_async_call(ui_update_ble_icon_async, NULL);
    lv_async_call(ui_update_ble_status_async, NULL);
}

static void clock_timer_cb(lv_timer_t *t)
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

    // Refresh notif bar if something dirtied it
    if (g_notif_dirty) {
        notif_icons_refresh();
        g_notif_dirty = false;
    }
}

/* ---------------- Notifications Bar ---------------- */

static void notif_bar_set_hidden_if_empty(void)
{
    if (!notif_bar) return;

    uint32_t total = 0;
    for (int i = 0; i < NOTIF_MAX; i++) total += g_notif_counts[i];

    if (total == 0) lv_obj_add_flag(notif_bar, LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_clear_flag(notif_bar, LV_OBJ_FLAG_HIDDEN);
}

static void notif_icons_refresh(void)
{
    if (!notif_bar) return;

    for (int i = 0; i < NOTIF_MAX; i++) {
        if (!notif_slot[i]) continue;

        uint16_t c = g_notif_counts[i];

        if (c == 0) {
            lv_obj_add_flag(notif_slot[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        lv_obj_clear_flag(notif_slot[i], LV_OBJ_FLAG_HIDDEN);

        // Badge: 1..99+
        char b[8];
        if (c > 99) strcpy(b, "99+");
        else snprintf(b, sizeof(b), "%u", (unsigned)c);

        lv_label_set_text(notif_badge_lbl[i], b);
    }

    notif_bar_set_hidden_if_empty();
}

void ui_notif_add(notif_type_t t)
{
    if (t < 0 || t >= NOTIF_MAX) return;
    if (g_notif_counts[t] < 999) g_notif_counts[t]++;

    g_notif_dirty = true;
    ui_notif_refresh_async();

    ESP_LOGI(UI_TAG, "notif add type=%d count=%u", (int)t, (unsigned)g_notif_counts[t]);
}

void ui_notif_clear_type(notif_type_t t)
{
    if (t < 0 || t >= NOTIF_MAX) return;
    g_notif_counts[t] = 0;

    g_notif_dirty = true;
    ui_notif_refresh_async();
}

void ui_notif_clear_all(void)
{
    for (int i = 0; i < NOTIF_MAX; i++) g_notif_counts[i] = 0;

    g_notif_dirty = true;
    ui_notif_refresh_async();
}

/* Call this from BLE thread safely */
static void ui_notif_add_async_cb(void *arg)
{
    notif_type_t t = (notif_type_t)(intptr_t)arg;
    ui_notif_add(t);
}

void ui_notif_add_from_ble(notif_type_t t)
{
    lv_async_call(ui_notif_add_async_cb, (void*)(intptr_t)t);
}

/* ---------------- WiFi Picker Modal ---------------- */

static void on_open_wifi_picker(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_wifi_picker_modal(lv_scr_act());
}

void open_wifi_picker_modal(lv_obj_t *parent)
{
    if (wifi_modal) return;

    wifi_modal = lv_obj_create(parent);
    lv_obj_set_size(wifi_modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(wifi_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(wifi_modal, LV_OPA_70, 0);
    lv_obj_clear_flag(wifi_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(wifi_modal);
    lv_obj_set_size(card, 360, 320);
    lv_obj_center(card);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Wifi Networks");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    wifi_list = lv_obj_create(card);
    lv_obj_set_size(wifi_list, lv_pct(90), 250);
    lv_obj_align(wifi_list, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_flex_flow(wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wifi_list, 6, 0);
    lv_obj_add_flag(wifi_list, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_close = lv_btn_create(card);
    lv_obj_set_size(btn_close, 90, 32);
    lv_obj_align(btn_close, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(btn_close, on_wifi_picker_close, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(btn_close), "Close");
    lv_obj_center(lv_obj_get_child(btn_close, 0));

    lv_obj_t *btn_scan = lv_btn_create(card);
    lv_obj_set_size(btn_scan, 90, 32);
    lv_obj_align(btn_scan, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(btn_scan, on_wifi_picker_scan, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(btn_scan), "Scan");
    lv_obj_center(lv_obj_get_child(btn_scan, 0));

    start_wifi_scan();
}

void close_wifi_picker_modal(void)
{
    if (wifi_modal) {
        lv_obj_del(wifi_modal);
        wifi_modal = NULL;
        wifi_list = NULL;
        wifi_status_lbl = NULL;
    }
}

void on_wifi_ap_clicked(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    const char *line = lv_label_get_text(lbl);
    if (!line) return;

    char ssid[33] = {0};
    int i = 0;
    while (line[i] && i < 32) {
        if (line[i] == ' ' && line[i+1] == ' ') break;
        ssid[i] = line[i];
        i++;
    }
    ssid[i] = '\0';

    snprintf(g_selected_ssid, sizeof(g_selected_ssid), "%s", ssid);
    open_wifi_password_modal(lv_scr_act(), g_selected_ssid);
}

void on_wifi_pass_cancel(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) close_wifi_password_modal();
}

void on_wifi_pass_connect(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    const char *pass = lv_textarea_get_text(wifi_pass_ta);
    if (!pass) pass = "";

    settings_save_wifi_creds(g_selected_ssid, pass);

    ESP_LOGI(UI_TAG, "Connecting to selected SSID...");
    wifi_start_sta(g_wifi_ssid, g_wifi_pass);

    close_wifi_password_modal();
}

void open_wifi_password_modal(lv_obj_t *parent, const char *ssid)
{
    if (wifi_pass_modal) return;

    wifi_pass_modal = lv_obj_create(parent);
    lv_obj_set_size(wifi_pass_modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(wifi_pass_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(wifi_pass_modal, LV_OPA_70, 0);
    lv_obj_clear_flag(wifi_pass_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(wifi_pass_modal);
    lv_obj_set_size(card, 300, 360);
    lv_obj_center(card);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    char title[64];
    snprintf(title, sizeof(title), "Join: %s", ssid ? ssid : "");
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 6);

    wifi_pass_ta = lv_textarea_create(card);
    lv_obj_set_width(wifi_pass_ta, 260);
    lv_obj_align(wifi_pass_ta, LV_ALIGN_TOP_MID, 0, 40);
    lv_textarea_set_password_mode(wifi_pass_ta, true);
    lv_textarea_set_one_line(wifi_pass_ta, true);
    lv_textarea_set_placeholder_text(wifi_pass_ta, "Password");

    wifi_kb = lv_keyboard_create(card);
    lv_obj_set_size(wifi_kb, 280, 180);
    lv_obj_align(wifi_kb, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_keyboard_set_textarea(wifi_kb, wifi_pass_ta);

    lv_obj_t *cancel = lv_btn_create(card);
    lv_obj_set_size(cancel, 90, 32);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(cancel, on_wifi_pass_cancel, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(cancel), "Cancel");
    lv_obj_center(lv_obj_get_child(cancel, 0));

    lv_obj_t *conn = lv_btn_create(card);
    lv_obj_set_size(conn, 90, 32);
    lv_obj_align(conn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(conn, on_wifi_pass_connect, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(conn), "Connect");
    lv_obj_center(lv_obj_get_child(conn, 0));
}

void close_wifi_password_modal(void)
{
    if (wifi_pass_modal) {
        lv_obj_del(wifi_pass_modal);
        wifi_pass_modal = NULL;
        wifi_pass_ta = NULL;
        wifi_kb = NULL;
    }
}

static void on_wifi_picker_close(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) close_wifi_picker_modal();
}

static void on_wifi_picker_scan(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) start_wifi_scan();
}

/* ---------------- Set Time Modal ---------------- */

static void settime_refresh_labels(void)
{
    if (!settime_h_lbl || !settime_m_lbl) return;

    static char buf[8];

    snprintf(buf, sizeof(buf), "%02d", set_h);
    lv_label_set_text(settime_h_lbl, buf);

    snprintf(buf, sizeof(buf), "%02d", set_m);
    lv_label_set_text(settime_m_lbl, buf);
}

static void settime_close(void)
{
    if (settime_modal) {
        lv_obj_del(settime_modal);
        settime_modal = NULL;
        settime_h_lbl = NULL;
        settime_m_lbl = NULL;
    }
}

static void on_settime_cancel(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) settime_close();
}

static void on_settime_save(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        set_system_time_hm(set_h, set_m);
        clock_update_label_now();
        settime_close();
    }
}

static void on_inc_hour(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        set_h = (set_h + 1) % 24;
        settime_refresh_labels();
    }
}

static void on_dec_hour(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        set_h = (set_h + 23) % 24;
        settime_refresh_labels();
    }
}

static void on_inc_min(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        set_m = (set_m + 1) % 60;
        settime_refresh_labels();
    }
}

static void on_dec_min(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        set_m = (set_m + 59) % 60;
        settime_refresh_labels();
    }
}

static void open_settime_modal(lv_obj_t *parent)
{
    if (settime_modal) return;

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    set_h = tm_now.tm_hour;
    set_m = tm_now.tm_min;

    settime_modal = lv_obj_create(parent);
    lv_obj_set_size(settime_modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(settime_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(settime_modal, LV_OPA_60, 0);
    lv_obj_clear_flag(settime_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(settime_modal);
    lv_obj_set_size(card, 280, 220);
    lv_obj_center(card);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Set Time");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, -10);

    lv_obj_t *h_plus = lv_btn_create(card);
    lv_obj_set_size(h_plus, 40, 40);
    lv_obj_align(h_plus, LV_ALIGN_LEFT_MID, 50, -40);
    lv_obj_add_event_cb(h_plus, on_inc_hour, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(h_plus), "+");
    lv_obj_center(lv_obj_get_child(h_plus, 0));

    lv_obj_t *h_minus = lv_btn_create(card);
    lv_obj_set_size(h_minus, 40, 40);
    lv_obj_align(h_minus, LV_ALIGN_LEFT_MID, 50, 40);
    lv_obj_add_event_cb(h_minus, on_dec_hour, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(h_minus), "-");
    lv_obj_center(lv_obj_get_child(h_minus, 0));

    settime_h_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(settime_h_lbl, &lv_font_montserrat_32, 0);
    lv_obj_align(settime_h_lbl, LV_ALIGN_LEFT_MID, 50, 0);

    lv_obj_t *colon = lv_label_create(card);
    lv_label_set_text(colon, ":");
    lv_obj_set_style_text_font(colon, &lv_font_montserrat_32, 0);
    lv_obj_align(colon, LV_ALIGN_CENTER, -5, 0);

    lv_obj_t *m_plus = lv_btn_create(card);
    lv_obj_set_size(m_plus, 40, 40);
    lv_obj_align(m_plus, LV_ALIGN_RIGHT_MID, -50, -40);
    lv_obj_add_event_cb(m_plus, on_inc_min, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(m_plus), "+");
    lv_obj_center(lv_obj_get_child(m_plus, 0));

    lv_obj_t *m_minus = lv_btn_create(card);
    lv_obj_set_size(m_minus, 40, 40);
    lv_obj_align(m_minus, LV_ALIGN_RIGHT_MID, -50, 40);
    lv_obj_add_event_cb(m_minus, on_dec_min, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(m_minus), "-");
    lv_obj_center(lv_obj_get_child(m_minus, 0));

    settime_m_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(settime_m_lbl, &lv_font_montserrat_32, 0);
    lv_obj_align(settime_m_lbl, LV_ALIGN_RIGHT_MID, -50, 0);

    lv_obj_t *cancel = lv_btn_create(card);
    lv_obj_set_size(cancel, 85, 30);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, -10, 10);
    lv_obj_add_event_cb(cancel, on_settime_cancel, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(cancel), "Cancel");
    lv_obj_center(lv_obj_get_child(cancel, 0));

    lv_obj_t *save = lv_btn_create(card);
    lv_obj_set_size(save, 85, 30);
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, 10, 10);
    lv_obj_add_event_cb(save, on_settime_save, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(save), "Save");
    lv_obj_center(lv_obj_get_child(save, 0));

    settime_refresh_labels();
}

static void on_open_settime(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        open_settime_modal(lv_scr_act());
    }
}

/* ---------------- Brightness Modal ---------------- */

static lv_obj_t *brightness_modal = NULL;

static void brightness_modal_close(lv_event_t *e)
{
    (void)e;
    if (brightness_modal) {
        lv_obj_del(brightness_modal);
        brightness_modal = NULL;
    }
}

static void open_brightness_modal(lv_obj_t *parent)
{
    if (brightness_modal) return;

    brightness_modal = lv_obj_create(parent);
    lv_obj_set_size(brightness_modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(brightness_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(brightness_modal, LV_OPA_70, 0);
    lv_obj_clear_flag(brightness_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(brightness_modal);
    lv_obj_set_size(card, 340, 220);
    lv_obj_center(card);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, "Brightness");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 8);

    brightness_slider = lv_slider_create(card);
    lv_obj_set_width(brightness_slider, lv_pct(90));
    lv_obj_align(brightness_slider, LV_ALIGN_CENTER, 0, 15);
    lv_slider_set_range(brightness_slider, 10, 100);
    lv_slider_set_value(brightness_slider, g_brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(brightness_slider, on_brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *close = lv_btn_create(card);
    lv_obj_set_size(close, 90, 36);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_event_cb(close, brightness_modal_close, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(close), "Close");
    lv_obj_center(lv_obj_get_child(close, 0));
}

static void on_open_brightness(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_brightness_modal(lv_scr_act());
}

/* ---------------- Tile Helpers (tabbed, non-scrollable, no visible switches) ---------------- */

#define TILE_W   130
#define TILE_H   100
#define TILE_RAD 13

static lv_color_t TILE_OFF_BG(void) { return lv_color_hex(0x2A2A2A); }  // grey
static lv_color_t TILE_ON_BG(void)  { return lv_color_hex(0x1E6BFF); }  // blue
static lv_color_t TILE_BORDER(void) { return lv_color_hex(0x3A3A3A); }

typedef struct {
    lv_obj_t *tile_btn;   // visible tile
    lv_obj_t *hidden_sw;  // hidden switch used only to reuse callbacks
    lv_obj_t *sub_lbl;    // optional subtitle label
} tile_switch_ctx_t;

static void tile_style_base(lv_obj_t *btn)
{
    lv_obj_set_style_radius(btn, TILE_RAD, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, TILE_BORDER(), 0);
    lv_obj_set_style_pad_all(btn, 12, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
}

static void tile_set_on(lv_obj_t *btn, bool on)
{
    lv_obj_set_style_bg_color(btn, on ? TILE_ON_BG() : TILE_OFF_BG(), 0);
}

static lv_obj_t *tile_create_base(lv_obj_t *parent,
                                 const char *title,
                                 const char *subtitle,
                                 lv_obj_t **out_sub_lbl)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, TILE_W, TILE_H);
    tile_style_base(btn);
    tile_set_on(btn, false);

    lv_obj_t *t = lv_label_create(btn);
    lv_label_set_text(t, title ? title : "");
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *s = lv_label_create(btn);
    lv_label_set_text(s, subtitle ? subtitle : "");
    lv_obj_set_style_text_color(s, lv_color_white(), 0);
    lv_obj_set_style_text_opa(s, LV_OPA_80, 0);
    lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    if (out_sub_lbl) *out_sub_lbl = s;
    return btn;
}

static lv_obj_t *tile_create_nav_tile(lv_obj_t *parent,
                                     const char *title,
                                     const char *subtitle,
                                     lv_event_cb_t on_click)
{
    lv_obj_t *sub = NULL;
    lv_obj_t *btn = tile_create_base(parent, title, subtitle, &sub);

    tile_set_on(btn, true); // nav tiles always blue

    if (on_click) lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, lv_color_white(), 0);
    lv_obj_set_style_text_opa(arrow, LV_OPA_70, 0);
    lv_obj_align(arrow, LV_ALIGN_TOP_RIGHT, 0, 0);

    return btn;
}

static void on_switch_tile_clicked(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    tile_switch_ctx_t *ctx = (tile_switch_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !ctx->tile_btn || !ctx->hidden_sw) return;

    bool now_on = !lv_obj_has_state(ctx->hidden_sw, LV_STATE_CHECKED);

    if (now_on) lv_obj_add_state(ctx->hidden_sw, LV_STATE_CHECKED);
    else        lv_obj_clear_state(ctx->hidden_sw, LV_STATE_CHECKED);

    tile_set_on(ctx->tile_btn, now_on);

    // reuse existing callback (expects VALUE_CHANGED on switch)
    lv_event_send(ctx->hidden_sw, LV_EVENT_VALUE_CHANGED, NULL);

    mark_user_activity();
}

static void on_tile_ctx_cleanup(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    tile_switch_ctx_t *ctx = (tile_switch_ctx_t *)lv_event_get_user_data(e);
    if (ctx) lv_mem_free(ctx);
}

static lv_obj_t *tile_create_switch_tile(lv_obj_t *parent,
                                        const char *title,
                                        const char *subtitle,
                                        bool initial_on,
                                        lv_event_cb_t on_switch_value_changed,
                                        lv_obj_t **out_sub_lbl)
{
    lv_obj_t *sub = NULL;
    lv_obj_t *btn = tile_create_base(parent, title, subtitle, &sub);

    // Hidden switch exists ONLY so existing callbacks still work
    lv_obj_t *sw = lv_switch_create(btn);
    lv_obj_add_flag(sw, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(sw, 1, 1);
    lv_obj_set_style_opa(sw, LV_OPA_0, 0);

    if (initial_on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else            lv_obj_clear_state(sw, LV_STATE_CHECKED);

    if (on_switch_value_changed) {
        lv_obj_add_event_cb(sw, on_switch_value_changed, LV_EVENT_VALUE_CHANGED, NULL);
    }

    tile_set_on(btn, initial_on);

    tile_switch_ctx_t *ctx = (tile_switch_ctx_t *)lv_mem_alloc(sizeof(tile_switch_ctx_t));
    ctx->tile_btn   = btn;
    ctx->hidden_sw  = sw;
    ctx->sub_lbl    = sub;

    lv_obj_add_event_cb(btn, on_switch_tile_clicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(btn, on_tile_ctx_cleanup,   LV_EVENT_DELETE,  ctx);

    if (out_sub_lbl) *out_sub_lbl = sub;
    return btn;
}

/* ---------------- Timeout tile ---------------- */

static uint32_t timeout_get_s(void)
{
    uint32_t s = g_screen_timeout_ms / 1000;
    if (s != 15 && s != 30 && s != 60) s = 15;
    return s;
}

static void timeout_update_subtitle(void)
{
    if (!timeout_sub_lbl) return;

    uint32_t s = timeout_get_s();
    static char buf[16];
    snprintf(buf, sizeof(buf), "%lus", (unsigned long)s);
    lv_label_set_text(timeout_sub_lbl, buf);
}

static void on_timeout_tile_clicked(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    uint32_t s = timeout_get_s();
    if (s == 15) s = 30;
    else if (s == 30) s = 60;
    else s = 15;

    g_screen_timeout_ms = s * 1000;
    settings_save_screen_timeout_s(s);

    timeout_update_subtitle();
    mark_user_activity();
}

/* ---------------- UI Callbacks ---------------- */

static void on_back_to_home(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) ui_show(UI_HOME);
}

static void on_open_clock(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) ui_show(UI_CLOCK);
}

static void on_open_settings(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) ui_show(UI_SETTINGS);
}

static void on_settings_back(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) ui_show(UI_HOME);
}

static void on_brightness_changed(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *slider = lv_event_get_target(e);
    g_brightness = lv_slider_get_value(slider);

    apply_backlight_percent(g_brightness);

    ESP_LOGI(UI_TAG, "Brightness=%d -> saving", g_brightness);
    settings_save_brightness(g_brightness);

    mark_user_activity();
}

static void on_time_format_changed(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *sw = lv_event_get_target(e);
    use_24h_format = lv_obj_has_state(sw, LV_STATE_CHECKED);

    ESP_LOGI(UI_TAG, "use24h toggled -> %d", (int)use_24h_format);
    settings_save_24h(use_24h_format);

    mark_user_activity();
}

static void on_wifi_toggle(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    g_wifi_on = on;
    settings_save_wifi(on);

    if (on) {
        if (g_wifi_ssid[0]) {
            ESP_LOGI(UI_TAG, "WiFi toggle ON -> connecting to saved SSID");
            wifi_start_sta(g_wifi_ssid, g_wifi_pass);
        } else {
            ESP_LOGI(UI_TAG, "WiFi toggle ON -> no saved SSID, open picker");
            open_wifi_picker_modal(lv_scr_act());
        }
    } else {
        ESP_LOGI(UI_TAG, "WiFi toggle OFF");
        wifi_stop();
    }

    mark_user_activity();
}

static void on_ble_toggle(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    g_ble_on = on;
    settings_save_ble(on);

    if (on) {
        ESP_LOGI(UI_TAG, "BLE toggle ON -> start advertising");
        ble_start();
    } else {
        ESP_LOGI(UI_TAG, "BLE toggle OFF -> stop");
        ble_stop();
        g_ble_connected = false;
        lv_async_call(ui_update_ble_status_async, NULL);
    }

    lv_async_call(ui_update_ble_icon_async, NULL);
    lv_async_call(ui_update_ble_status_async, NULL);

    mark_user_activity();
}

/* ---------------- Screens ---------------- */

static lv_obj_t *build_home_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_add_event_cb(scr, activity_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *btn_clock = lv_btn_create(scr);
    lv_obj_set_size(btn_clock, 180, 120);
    lv_obj_align(btn_clock, LV_ALIGN_CENTER, 0, -70);
    lv_obj_add_event_cb(btn_clock, on_open_clock, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(btn_clock), "Clock");
    lv_obj_center(lv_obj_get_child(btn_clock, 0));

    lv_obj_t *btn_settings = lv_btn_create(scr);
    lv_obj_set_size(btn_settings, 180, 120);
    lv_obj_align(btn_settings, LV_ALIGN_CENTER, 0, 70);
    lv_obj_add_event_cb(btn_settings, on_open_settings, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(btn_settings), "Settings");
    lv_obj_center(lv_obj_get_child(btn_settings, 0));

    return scr;
}

static lv_obj_t *build_clock_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, activity_event_cb, LV_EVENT_ALL, NULL);

    clock_time_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(clock_time_lbl, &orbitron_72, 0);
    lv_obj_set_style_text_color(clock_time_lbl, lv_color_white(), 0);
    lv_obj_align(clock_time_lbl, LV_ALIGN_TOP_MID, 0, 30);

    clock_date_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(clock_date_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(clock_date_lbl, lv_color_white(), 0);
    lv_obj_align(clock_date_lbl, LV_ALIGN_TOP_LEFT, 5, 5);

    clock_wifi_icon = lv_label_create(scr);
    lv_obj_set_style_text_color(clock_wifi_icon, lv_color_white(), 0);
    lv_obj_align(clock_wifi_icon, LV_ALIGN_TOP_RIGHT, -12, 12);
    lv_label_set_text(clock_wifi_icon, LV_SYMBOL_WIFI);

    clock_ble_icon = lv_label_create(scr);
    lv_obj_set_style_text_color(clock_ble_icon, lv_color_white(), 0);
    lv_obj_align_to(clock_ble_icon, clock_wifi_icon, LV_ALIGN_OUT_LEFT_MID, -10, 0);
    lv_label_set_text(clock_ble_icon, LV_SYMBOL_BLUETOOTH);

    clock_update_label_now();
    clock_update_wifi_icon_now();
    clock_update_ble_icon_now();

    if (clock_timer == NULL) {
        clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
    }

    lv_obj_t *menu = lv_btn_create(scr);
    lv_obj_set_size(menu, 90, 50);
    lv_obj_set_style_bg_color(menu, lv_color_black(), 0);
    lv_obj_align(menu, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    lv_obj_add_event_cb(menu, on_back_to_home, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(menu), LV_SYMBOL_HOME);
    lv_obj_center(lv_obj_get_child(menu, 0));

    // --- Notification icon bar (fixed layout) ---
    notif_bar = lv_obj_create(scr);
    lv_obj_set_size(notif_bar, lv_pct(92), NOTIF_ICON_H);
    lv_obj_align(notif_bar, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_set_style_bg_opa(notif_bar, LV_OPA_0, 0);
    lv_obj_set_style_border_width(notif_bar, 0, 0);
    lv_obj_clear_flag(notif_bar, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < NOTIF_MAX; i++) {
        notif_slot[i] = lv_obj_create(notif_bar);
        lv_obj_set_size(notif_slot[i], NOTIF_ICON_W, NOTIF_ICON_H);
        lv_obj_set_style_radius(notif_slot[i], 8, 0);
        lv_obj_set_style_bg_color(notif_slot[i], lv_color_hex(0x202020), 0);
        lv_obj_set_style_bg_opa(notif_slot[i], LV_OPA_70, 0);
        lv_obj_set_style_border_width(notif_slot[i], 1, 0);
        lv_obj_set_style_border_color(notif_slot[i], lv_color_hex(0x505050), 0);
        lv_obj_clear_flag(notif_slot[i], LV_OBJ_FLAG_SCROLLABLE);

        // manual X positioning
        lv_obj_align(notif_slot[i], LV_ALIGN_LEFT_MID,
                     i * (NOTIF_ICON_W + NOTIF_GAP), 0);

        // Icon label (centered)
        notif_icon_lbl[i] = lv_label_create(notif_slot[i]);
        lv_obj_set_style_text_color(notif_icon_lbl[i], lv_color_white(), 0);
        lv_obj_set_style_text_font(notif_icon_lbl[i], &lv_font_montserrat_16, 0);
        lv_label_set_text(notif_icon_lbl[i], "?");
        lv_obj_center(notif_icon_lbl[i]);

        // Badge label (top-right)
        notif_badge_lbl[i] = lv_label_create(notif_slot[i]);
        lv_obj_set_style_text_color(notif_badge_lbl[i], lv_color_white(), 0);
        lv_obj_set_style_text_font(notif_badge_lbl[i], &lv_font_montserrat_12, 0);
        lv_label_set_text(notif_badge_lbl[i], "");
        lv_obj_align(notif_badge_lbl[i], LV_ALIGN_TOP_RIGHT, 4, -6);

        lv_obj_add_flag(notif_slot[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Set icon glyphs ONCE (outside the loop)
    lv_label_set_text(notif_icon_lbl[NOTIF_SMS],   "S");
    lv_label_set_text(notif_icon_lbl[NOTIF_EMAIL], "E");
    lv_label_set_text(notif_icon_lbl[NOTIF_MSG],   "M");
    lv_label_set_text(notif_icon_lbl[NOTIF_APP],   "A");

    // Hide bar until there’s at least one notif
    lv_obj_add_flag(notif_bar, LV_OBJ_FLAG_HIDDEN);

    // If counts already exist (e.g., you rebooted and restored), reflect them
    notif_icons_refresh();

    return scr;
}

static lv_obj_t *build_settings_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, activity_event_cb, LV_EVENT_ALL, NULL);

    // TabView: Display / Network / Time&Date
    lv_obj_t *tv = lv_tabview_create(scr, LV_DIR_TOP, 48);
    lv_obj_set_size(tv, lv_pct(100), lv_pct(100));
    lv_obj_align(tv, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(tv, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, 0);

    // Content should NOT scroll
    lv_obj_t *content = lv_tabview_get_content(tv);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(content, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(content, 0, 0);

    lv_obj_t *tab_bar = lv_tabview_get_tab_btns(tv);
    lv_obj_set_style_bg_color(tab_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tab_bar, 0, 0);
    lv_obj_set_style_pad_all(tab_bar, 6, 0);

    lv_obj_set_style_text_color(tab_bar, lv_color_hex(0xAAAAAA), LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(tab_bar, lv_color_white(),      LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_0,  LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_20, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_radius(tab_bar, 10, LV_PART_ITEMS);

    lv_obj_t *tab_display = lv_tabview_add_tab(tv, "Display");
    lv_obj_t *tab_network = lv_tabview_add_tab(tv, "Network");
    lv_obj_t *tab_time    = lv_tabview_add_tab(tv, "Time/Date");

    static lv_coord_t col_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static lv_coord_t row_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };

    // DISPLAY GRID
    lv_obj_t *g_disp = lv_obj_create(tab_display);
    lv_obj_set_size(g_disp, lv_pct(100), lv_pct(100));
    lv_obj_center(g_disp);
    lv_obj_set_style_bg_opa(g_disp, LV_OPA_0, 0);
    lv_obj_set_style_border_width(g_disp, 0, 0);
    lv_obj_set_style_pad_all(g_disp, 14, 0);
    lv_obj_set_style_pad_row(g_disp, 14, 0);
    lv_obj_set_style_pad_column(g_disp, 14, 0);
    lv_obj_clear_flag(g_disp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_grid_dsc_array(g_disp, col_dsc, row_dsc);

    lv_obj_t *t_bright = tile_create_nav_tile(g_disp, "Brightness", "Adjust", on_open_brightness);
    lv_obj_set_grid_cell(t_bright, LV_GRID_ALIGN_CENTER, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);

    lv_obj_t *t_timeout = tile_create_base(g_disp, "Timeout", "", &timeout_sub_lbl);
    tile_set_on(t_timeout, true);
    lv_obj_set_grid_cell(t_timeout, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    timeout_update_subtitle();
    lv_obj_add_event_cb(t_timeout, on_timeout_tile_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t_placeholder1 = tile_create_base(g_disp, "Theme", "Later", NULL);
    tile_set_on(t_placeholder1, false);
    lv_obj_set_grid_cell(t_placeholder1, LV_GRID_ALIGN_CENTER, 0, 1, LV_GRID_ALIGN_CENTER, 1, 1);

    lv_obj_t *t_placeholder2 = tile_create_base(g_disp, "Always On", "Later", NULL);
    tile_set_on(t_placeholder2, false);
    lv_obj_set_grid_cell(t_placeholder2, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 1, 1);

    // NETWORK GRID
    lv_obj_t *g_net = lv_obj_create(tab_network);
    lv_obj_set_size(g_net, lv_pct(100), lv_pct(100));
    lv_obj_center(g_net);
    lv_obj_set_style_bg_opa(g_net, LV_OPA_0, 0);
    lv_obj_set_style_border_width(g_net, 0, 0);
    lv_obj_set_style_pad_all(g_net, 14, 0);
    lv_obj_set_style_pad_row(g_net, 14, 0);
    lv_obj_set_style_pad_column(g_net, 14, 0);
    lv_obj_clear_flag(g_net, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_grid_dsc_array(g_net, col_dsc, row_dsc);

    lv_obj_t *wifi_sub = NULL;
    lv_obj_t *t_wifi = tile_create_switch_tile(g_net, "WiFi", g_ip_str, g_wifi_on, on_wifi_toggle, &wifi_sub);
    lv_obj_set_grid_cell(t_wifi, LV_GRID_ALIGN_CENTER, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    ip_lbl = wifi_sub;

    lv_obj_t *t_networks = tile_create_nav_tile(
        g_net, "Networks",
        (g_wifi_ssid[0] != '\0') ? g_wifi_ssid : "Choose WiFi",
        on_open_wifi_picker
    );
    lv_obj_set_grid_cell(t_networks, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);

    lv_obj_t *ble_sub = NULL;
    lv_obj_t *t_ble = tile_create_switch_tile(g_net, "BLE", "Off", g_ble_on, on_ble_toggle, &ble_sub);
    lv_obj_set_grid_cell(t_ble, LV_GRID_ALIGN_CENTER, 0, 1, LV_GRID_ALIGN_CENTER, 1, 1);
    ble_status_lbl = ble_sub;
    lv_async_call(ui_update_ble_status_async, NULL);

    lv_obj_t *t_notif = tile_create_nav_tile(g_net, "Notifications", "Coming soon", NULL);
    lv_obj_set_grid_cell(t_notif, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 1, 1);

    // TIME/DATE GRID
    lv_obj_t *g_time = lv_obj_create(tab_time);
    lv_obj_set_size(g_time, lv_pct(100), lv_pct(100));
    lv_obj_center(g_time);
    lv_obj_set_style_bg_opa(g_time, LV_OPA_0, 0);
    lv_obj_set_style_border_width(g_time, 0, 0);
    lv_obj_set_style_pad_all(g_time, 14, 0);
    lv_obj_set_style_pad_row(g_time, 14, 0);
    lv_obj_set_style_pad_column(g_time, 14, 0);
    lv_obj_clear_flag(g_time, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_grid_dsc_array(g_time, col_dsc, row_dsc);

    lv_obj_t *t_24h = tile_create_switch_tile(g_time, "24h Time", "", use_24h_format, on_time_format_changed, NULL);
    lv_obj_set_grid_cell(t_24h, LV_GRID_ALIGN_CENTER, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);

    lv_obj_t *t_set = tile_create_nav_tile(g_time, "Set Time", "Manual", on_open_settime);
    lv_obj_set_grid_cell(t_set, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);

    lv_obj_t *t_date = tile_create_base(g_time, "Date", "Later", NULL);
    tile_set_on(t_date, false);
    lv_obj_set_grid_cell(t_date, LV_GRID_ALIGN_CENTER, 0, 1, LV_GRID_ALIGN_CENTER, 1, 1);

    lv_obj_t *t_sync = tile_create_base(g_time, "Sync", "SNTP", NULL);
    tile_set_on(t_sync, true);
    lv_obj_set_grid_cell(t_sync, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 1, 1);

    // Top-left back button (overlay)
    lv_obj_t *back = lv_btn_create(scr);
    lv_obj_set_size(back, 20, 20);
    lv_obj_set_style_radius(back, 4, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(back, 0, 0);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_add_event_cb(back, on_settings_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btxt = lv_label_create(back);
    lv_label_set_text(btxt, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(btxt, lv_color_white(), 0);
    lv_obj_center(btxt);

    return scr;
}

static lv_obj_t *build_black_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

/* ---------------- Router ---------------- */

void ui_show(ui_screen_t s)
{
    if (s == UI_HOME) {
        if (!scr_home) scr_home = build_home_screen();
        lv_scr_load(scr_home);

    } else if (s == UI_CLOCK) {
        if (!scr_clock) scr_clock = build_clock_screen();
        lv_scr_load(scr_clock);

        // reflect current state
        clock_update_label_now();
        notif_icons_refresh();
        g_notif_dirty = false;

    } else if (s == UI_SETTINGS) {
        if (scr_settings) {
            lv_obj_del(scr_settings);
            scr_settings = NULL;
        }
        if (!scr_settings) scr_settings = build_settings_screen();
        lv_scr_load(scr_settings);
        ui_update_ip_label();

    } else if (s == UI_BLANK) {
        if (!scr_blank) scr_blank = build_black_screen();
        lv_scr_load(scr_blank);
        return;
    }
}

void create_watch_ui(void)
{
    ui_show(UI_CLOCK);
}
