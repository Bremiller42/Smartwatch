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
#include "watch_sdcard.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

static lv_obj_t   *s_bar_sd     = NULL;
static lv_obj_t   *s_lbl_sd_txt = NULL;
static lv_obj_t   *s_sd_row     = NULL;

/* Modal */
static lv_obj_t   *s_sd_msgbox  = NULL;

/* ---------------- Screen-timeout override ---------------- */
static void device_info_enter_always_on(void);
static void device_info_exit_always_on(void);

/* ---------------- Events ---------------- */
static void on_back(lv_event_t *e);
static void on_screen_delete(lv_event_t *e);

/* SD row / modal */
static void on_sd_row_clicked(lv_event_t *e);
static void sd_msgbox_btn_cb(lv_event_t *e);

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

/* Always parent modals to the ACTIVE screen (never NULL). */
static inline lv_obj_t *ui_parent_screen(void)
{
    return lv_scr_act();
}

/* Close msgbox in a way that also releases indev capture (prevents stuck modal overlay). */
static void sd_ui_close_msgbox(void)
{
    if (!s_sd_msgbox) return;

    lv_indev_t *indev = lv_indev_get_act();
    if (indev) {
        lv_indev_reset(indev, s_sd_msgbox);
    }

    /* Sync delete (we are in LVGL context when this is called) */
    lv_obj_del(s_sd_msgbox);
    s_sd_msgbox = NULL;
}

/* ---------------- UI builders ---------------- */

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

/* ---------------- SD unmount async flow ---------------- */

typedef struct {
    uint32_t timeout_ms;
} sd_unmount_job_t;

/* Show a simple OK modal (must be called on LVGL thread). */
static void sd_ui_show_ok(const char *msg)
{
    static const char *ok[] = { "OK", "" };

    sd_ui_close_msgbox(); /* just in case */

    s_sd_msgbox = lv_msgbox_create(ui_parent_screen(), "SD Card", msg, ok, true);
    lv_obj_center(s_sd_msgbox);

    lv_obj_t *btnm = lv_msgbox_get_btns(s_sd_msgbox);
    lv_obj_add_event_cb(btnm, sd_msgbox_btn_cb, LV_EVENT_CLICKED, NULL);
}

/* Unmount done callback (LVGL thread via lv_async_call). */
static void sd_unmount_done_cb(void *p)
{
    esp_err_t err = (esp_err_t)(intptr_t)p;

    /* Close progress box if present */
    sd_ui_close_msgbox();

    if (err == ESP_OK) sd_ui_show_ok("Unmounted.");
    else              sd_ui_show_ok("Unmount failed.");
}

static void sd_unmount_task(void *arg)
{
    sd_unmount_job_t *job = (sd_unmount_job_t *)arg;
    esp_err_t err = watch_sdcard_request_unmount(job ? job->timeout_ms : 5000);

    /* Post result back to LVGL thread */
    lv_async_call(sd_unmount_done_cb, (void *)(intptr_t)err);

    if (job) free(job);
    vTaskDelete(NULL);
}

static void on_sd_row_clicked(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_sd_msgbox) return;

    if (!watch_sdcard_is_mounted()) {
        static const char *btns[] = { "OK", "" };
        s_sd_msgbox = lv_msgbox_create(ui_parent_screen(), "SD Card", "SD card is not mounted.", btns, true);
        lv_obj_center(s_sd_msgbox);

        lv_obj_t *btnm = lv_msgbox_get_btns(s_sd_msgbox);
        lv_obj_add_event_cb(btnm, sd_msgbox_btn_cb, LV_EVENT_CLICKED, NULL);
        return;
    }

    static const char *btns[] = { "Unmount", "Cancel", "" };
    s_sd_msgbox = lv_msgbox_create(
        ui_parent_screen(),
        "Unmount SD Card",
        "Safely unmount the SD card?\nAny active writes will stop.",
        btns,
        true
    );
    lv_obj_center(s_sd_msgbox);

    lv_obj_t *btnm = lv_msgbox_get_btns(s_sd_msgbox);
    lv_obj_add_event_cb(btnm, sd_msgbox_btn_cb, LV_EVENT_CLICKED, NULL);
}

static void sd_msgbox_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!s_sd_msgbox) return;

    const char *txt = lv_msgbox_get_active_btn_text(s_sd_msgbox);
    if (!txt) txt = "";

    /* Copy selection before we delete the box */
    char choice[16];
    strncpy(choice, txt, sizeof(choice) - 1);
    choice[sizeof(choice) - 1] = '\0';

    /* Close current modal fully (including bg) */
    sd_ui_close_msgbox();

    if (strcmp(choice, "Unmount") == 0) {
        /* Show a non-modal progress box (no input blocking) */
        static const char *none[] = { "", "" };
        s_sd_msgbox = lv_msgbox_create(ui_parent_screen(), "SD Card", "Unmounting...", none, false);
        lv_obj_center(s_sd_msgbox);

        sd_unmount_job_t *job = (sd_unmount_job_t *)malloc(sizeof(sd_unmount_job_t));
        if (job) job->timeout_ms = 5000;

        xTaskCreate(sd_unmount_task, "sd_unmount_ui", 3072, job, 5, NULL);
        return;
    }

    /* OK/Cancel: nothing else to do */
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
    sd_ui_close_msgbox();

    s_title = NULL;
    s_rows_cont = NULL;

    s_lbl_chip = NULL;
    s_lbl_id = NULL;
    s_lbl_uptime = NULL;
    s_lbl_flash = NULL;
    s_lbl_fw = NULL;

    s_lbl_heap_txt = NULL;
    s_lbl_psram_txt = NULL;
    s_bar_heap = NULL;
    s_bar_psram = NULL;

    s_bar_sd = NULL;
    s_lbl_sd_txt = NULL;
    s_sd_row = NULL;

    s_lbl_batt = NULL;
    s_lbl_drain = NULL;

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

    /* SD Card bar + text (cached; UI thread safe) */
    if (s_bar_sd && s_lbl_sd_txt) {
        if (!watch_sdcard_is_mounted()) {
            lv_bar_set_value(s_bar_sd, 0, LV_ANIM_OFF);
            lv_label_set_text(s_lbl_sd_txt, "unmounted");
        } else {
            uint64_t total_kb = watch_sdcard_total_kb_cached();
            uint64_t free_kb  = watch_sdcard_free_kb_cached();
            uint64_t used_kb  = (total_kb >= free_kb) ? (total_kb - free_kb) : 0;

            int used_pct = 0;
            if (total_kb > 0) {
                used_pct = (int)((used_kb * 100ULL) / total_kb);
                if (used_pct < 0) used_pct = 0;
                if (used_pct > 100) used_pct = 100;
            }

            lv_bar_set_value(s_bar_sd, used_pct, LV_ANIM_OFF);

            char line[128];
            uint64_t total_mb = total_kb / 1024;
            uint64_t free_mb  = free_kb / 1024;

            snprintf(line, sizeof(line),
                     "%s (%s)\n%llu MB free / %llu MB",
                     watch_sdcard_mount_point(),
                     watch_sdcard_id_cached(),
                     (unsigned long long)free_mb,
                     (unsigned long long)total_mb);

            lv_label_set_text(s_lbl_sd_txt, line);
        }
    }

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

    /* Battery */
    if (g_watch_batt_pct < 0 || g_watch_batt_v < 0.0f) {
        lv_label_set_text(s_lbl_batt, "--");
    } else {
        snprintf(buf, sizeof(buf), "%d%%, %.2f V", g_watch_batt_pct, g_watch_batt_v);
        lv_label_set_text(s_lbl_batt, buf);
    }

    /* Drain / ETA */
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

    s_sd_row = make_bar_row(s_rows_cont, "SD Card", &s_bar_sd, &s_lbl_sd_txt);
    lv_obj_add_flag(s_sd_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_sd_row, on_sd_row_clicked, LV_EVENT_CLICKED, NULL);

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

    device_info_enter_always_on();
    return scr;
}
