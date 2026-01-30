// FILE: src/ui/ui_screen_device_info.c
// Device Info / About screen (LVGL8)

#include "lvgl.h"
#include "ui_priv.h"

#include "ui_color_pallete.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "watch_globals.h"
#include "watch_screen_timeout.h"
#include "ui_priv.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

/* Battery / drain globals */
extern int   g_watch_batt_pct;
extern float g_watch_batt_v;
extern float g_batt_drain_pct_per_hr;
extern float g_batt_drain_mv_per_hr;
extern int   g_batt_eta_min;

#define DEV_REFRESH_MS 1000

/* ---------------- UI objects ---------------- */
static lv_obj_t   *s_title      = NULL;
static lv_obj_t   *s_rows_cont  = NULL;
static lv_obj_t   *s_lbl_chip   = NULL;
static lv_obj_t   *s_lbl_id     = NULL;
static lv_obj_t   *s_lbl_uptime = NULL;
static lv_obj_t   *s_lbl_flash  = NULL;
static lv_obj_t   *s_lbl_fw     = NULL;

static lv_obj_t   *s_lbl_heap_txt   = NULL;
static lv_obj_t   *s_lbl_psram_txt  = NULL;
static lv_obj_t   *s_bar_heap       = NULL;
static lv_obj_t   *s_bar_psram      = NULL;

static lv_obj_t   *s_lbl_batt   = NULL;
static lv_obj_t   *s_lbl_drain  = NULL;

static lv_timer_t *s_timer = NULL;

/* Screen-timeout override token (matches log screen pattern) */
static void device_info_enter_always_on(void);
static void device_info_exit_always_on(void);
static void on_screen_delete(lv_event_t *e);

/* ---------------- Helpers ---------------- */

static void fmt_uptime(char *out, size_t out_sz, int64_t ms)
{
    int64_t sec = ms / 1000;
    int64_t min = sec / 60;  sec %= 60;
    int64_t hr  = min / 60;  min %= 60;
    int64_t day = hr  / 24;  hr  %= 24;

    if (day > 0) {
        snprintf(out, out_sz, "%" PRId64 "d %" PRId64 "h %" PRId64 "m", day, hr, min);
    } else if (hr > 0) {
        snprintf(out, out_sz, "%" PRId64 "h %" PRId64 "m %" PRId64 "s", hr, min, sec);
    } else {
        snprintf(out, out_sz, "%" PRId64 "m %" PRId64 "s", min, sec);
    }
}    
static void device_info_enter_always_on(void)
{
    screen_keep_awake_acquire();
    screen_timeout_mark_activity();
}

static void device_info_exit_always_on(void)
{
    screen_keep_awake_release();
    screen_timeout_mark_activity();
}


static const char *chip_model_str(esp_chip_model_t m)
{
    switch (m) {
        case CHIP_ESP32:    return "ESP32";
        case CHIP_ESP32S2:  return "ESP32-S2";
        case CHIP_ESP32S3:  return "ESP32-S3";
        case CHIP_ESP32C3:  return "ESP32-C3";
        case CHIP_ESP32C2:  return "ESP32-C2";
        case CHIP_ESP32C6:  return "ESP32-C6";
        case CHIP_ESP32H2:  return "ESP32-H2";
        default:            return "ESP32(?)";
    }
}

/* Compact text row */
static lv_obj_t *make_row(lv_obj_t *parent, const char *title, lv_obj_t **out_value_label)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));

    lv_obj_set_style_bg_color(card, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_70, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x303030), 0);
    lv_obj_set_style_radius(card, 12, 0);

    lv_obj_set_style_pad_left(card, 10, 0);
    lv_obj_set_style_pad_right(card, 10, 0);
    lv_obj_set_style_pad_top(card, 6, 0);
    lv_obj_set_style_pad_bottom(card, 6, 0);
    lv_obj_set_style_pad_row(card, 2, 0);

    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, 44, 0);

    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_font(t, LV_FONT_DEFAULT, 0);

    lv_obj_t *v = lv_label_create(card);
    lv_label_set_text(v, "--");
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v, lv_pct(100));
    lv_obj_set_style_text_color(v, UI_COLOR(THEME), 0);
    lv_obj_set_style_text_font(v, LV_FONT_DEFAULT, 0);

    if (out_value_label) *out_value_label = v;
    return card;
}

/* Bar row: title + bar + text */
static lv_obj_t *make_bar_row(lv_obj_t *parent, const char *title,
                              lv_obj_t **out_bar, lv_obj_t **out_text_label)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));

    lv_obj_set_style_bg_color(card, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_70, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x303030), 0);
    lv_obj_set_style_radius(card, 12, 0);

    lv_obj_set_style_pad_left(card, 10, 0);
    lv_obj_set_style_pad_right(card, 10, 0);
    lv_obj_set_style_pad_top(card, 6, 0);
    lv_obj_set_style_pad_bottom(card, 6, 0);
    lv_obj_set_style_pad_row(card, 4, 0);

    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, 52, 0);

    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_font(t, LV_FONT_DEFAULT, 0);

    lv_obj_t *bar = lv_bar_create(card);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, 10);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);

    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, UI_COLOR(THEME), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);

    lv_obj_t *txt = lv_label_create(card);
    lv_label_set_text(txt, "--");
    lv_obj_set_style_text_color(txt, UI_COLOR(THEME), 0);
    lv_obj_set_style_text_font(txt, LV_FONT_DEFAULT, 0);

    if (out_bar) *out_bar = bar;
    if (out_text_label) *out_text_label = txt;
    return card;
}

/* ---------------- Events ---------------- */

static void on_back(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    device_info_exit_always_on();
    ui_show(UI_HOME);
}


static void on_screen_delete(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;

    device_info_exit_always_on();

    s_title = NULL;
    s_rows_cont = NULL;

    s_lbl_chip = NULL;
    s_lbl_id = NULL;
    s_lbl_uptime = NULL;
    s_lbl_flash = NULL;

    s_lbl_heap_txt = NULL;
    s_lbl_psram_txt = NULL;
    s_bar_heap = NULL;
    s_bar_psram = NULL;

    s_lbl_batt = NULL;
    s_lbl_drain = NULL;
    s_lbl_fw = NULL;

    if (s_timer) lv_timer_pause(s_timer);
}


/* ---------------- Refresh ---------------- */

static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_lbl_chip) return;

    esp_chip_info_t ci;
    esp_chip_info(&ci);

    char buf[256];

    if (s_lbl_fw) {
        lv_label_set_text(s_lbl_fw, FW_VERSION ? FW_VERSION : "unknown");
    }

    snprintf(buf, sizeof(buf), "%s, %d core(s), rev %d",
             chip_model_str(ci.model), ci.cores, ci.revision);
    lv_label_set_text(s_lbl_chip, buf);

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(buf, sizeof(buf), "unknown");
    }
    lv_label_set_text(s_lbl_id, buf);

    int64_t now_ms = esp_timer_get_time() / 1000;
    char up[64];
    fmt_uptime(up, sizeof(up), now_ms);
    lv_label_set_text(s_lbl_uptime, up);

    uint32_t flash_bytes = 0;
    (void)esp_flash_get_size(NULL, &flash_bytes);
    if (flash_bytes > 0) snprintf(buf, sizeof(buf), "%.1f MB", (double)flash_bytes / (1024.0 * 1024.0));
    else                 snprintf(buf, sizeof(buf), "unknown");
    lv_label_set_text(s_lbl_flash, buf);

    /* Internal heap */
    size_t int_total  = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t int_free   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t int_used   = (int_total >= int_free) ? (int_total - int_free) : 0;

    int int_pct = 0;
    if (int_total > 0) {
        int_pct = (int)((int_used * 100ULL) / int_total);
        if (int_pct < 0) int_pct = 0;
        if (int_pct > 100) int_pct = 100;
    }

    if (s_bar_heap) lv_bar_set_value(s_bar_heap, int_pct, LV_ANIM_OFF);
    if (s_lbl_heap_txt) {
        snprintf(buf, sizeof(buf), "%u KB / %u KB (%d%% used)",
                 (unsigned)(int_used / 1024), (unsigned)(int_total / 1024), int_pct);
        lv_label_set_text(s_lbl_heap_txt, buf);
    }

    /* PSRAM */
    size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t ps_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t ps_used  = (ps_total >= ps_free) ? (ps_total - ps_free) : 0;

    if (ps_total == 0) {
        if (s_lbl_psram_txt) lv_label_set_text(s_lbl_psram_txt, "not detected / not enabled");
        if (s_bar_psram) lv_bar_set_value(s_bar_psram, 0, LV_ANIM_OFF);
    } else {
        int ps_pct = (int)((ps_used * 100ULL) / ps_total);
        if (ps_pct < 0) ps_pct = 0;
        if (ps_pct > 100) ps_pct = 100;

        if (s_bar_psram) lv_bar_set_value(s_bar_psram, ps_pct, LV_ANIM_OFF);
        if (s_lbl_psram_txt) {
            snprintf(buf, sizeof(buf), "%u KB / %u KB (%d%% used)",
                     (unsigned)(ps_used / 1024), (unsigned)(ps_total / 1024), ps_pct);
            lv_label_set_text(s_lbl_psram_txt, buf);
        }
    }

    if (g_watch_batt_pct < 0 || g_watch_batt_v < 0.0f) {
        lv_label_set_text(s_lbl_batt, "--");
    } else {
        snprintf(buf, sizeof(buf), "%d%%, %.2f V", g_watch_batt_pct, g_watch_batt_v);
        lv_label_set_text(s_lbl_batt, buf);
    }

    if (g_batt_eta_min > 0) {
        int hr = g_batt_eta_min / 60;
        int mn = g_batt_eta_min % 60;
        snprintf(buf, sizeof(buf), "%.2f%%/hr, %.0f mV/hr, ETA %dh %dm",
                 g_batt_drain_pct_per_hr, g_batt_drain_mv_per_hr, hr, mn);
    } else {
        snprintf(buf, sizeof(buf), "%.2f%%/hr, %.0f mV/hr",
                 g_batt_drain_pct_per_hr, g_batt_drain_mv_per_hr);
    }
    lv_label_set_text(s_lbl_drain, buf);
}

/* ---------------- Build screen ---------------- */

lv_obj_t *ui_build_device_info_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_add_event_cb(scr, on_screen_delete, LV_EVENT_DELETE, NULL);

    /* back button */
    lv_obj_t *back = lv_btn_create(scr);
    lv_obj_set_size(back, 25, 25);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_60, 0);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btxt = lv_label_create(back);
    lv_label_set_text(btxt, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(btxt, UI_COLOR(RED), 0);
    lv_obj_center(btxt);

    /* title */
    s_title = lv_label_create(scr);
    lv_label_set_text(s_title, "Device Info");
    lv_obj_set_style_text_color(s_title, UI_COLOR(THEME), 0);
    lv_obj_set_style_text_font(s_title, LV_FONT_DEFAULT, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 10);

    /* rows container */
    s_rows_cont = lv_obj_create(scr);
    lv_obj_set_size(s_rows_cont, lv_pct(96), lv_pct(86));
    lv_obj_align(s_rows_cont, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_opa(s_rows_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_rows_cont, 0, 0);
    lv_obj_set_style_pad_all(s_rows_cont, 0, 0);
    lv_obj_set_style_pad_row(s_rows_cont, 10, 0);

    lv_obj_set_flex_flow(s_rows_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_rows_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    
    make_row(s_rows_cont, "Firmware", &s_lbl_fw);
    make_row(s_rows_cont, "Chip", &s_lbl_chip);
    make_row(s_rows_cont, "Chip ID (MAC)", &s_lbl_id);
    make_row(s_rows_cont, "Uptime", &s_lbl_uptime);
    make_row(s_rows_cont, "Flash", &s_lbl_flash);

    make_bar_row(s_rows_cont, "Internal Heap", &s_bar_heap, &s_lbl_heap_txt);
    make_bar_row(s_rows_cont, "PSRAM", &s_bar_psram, &s_lbl_psram_txt);

    make_row(s_rows_cont, "Battery", &s_lbl_batt);
    make_row(s_rows_cont, "Drain / ETA", &s_lbl_drain);

    /* timer */
    if (!s_timer) s_timer = lv_timer_create(refresh_cb, DEV_REFRESH_MS, NULL);
    else          lv_timer_set_period(s_timer, DEV_REFRESH_MS);
    lv_timer_resume(s_timer);

    refresh_cb(NULL);

    /* EXACTLY like log screen: push override at end of build */
    device_info_enter_always_on();


    return scr;
}
