// FILE: src/ui/ui_screen_log.c
// Goal: stable + cheap log viewer
// Key rules:
//  - NEVER call lv_textarea_add_text() many times per tick (WDT risk)
//  - Append at most ONCE per refresh tick (batch into one buffer)
//  - Trim rarely (set_text is expensive)
//  - Only move cursor when we actually appended AND user is at bottom

#include "lvgl.h"
#include "ui_priv.h"
#include "watch_logstream.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ---------------- State ---------------- */
static lv_obj_t   *s_log_ta    = NULL;
static lv_timer_t *s_log_timer = NULL;

/* partial line carry between ticks (optional but helps) */
static char   s_line_acc[512];
static size_t s_line_acc_len = 0;


/* ---------------- Tune ---------------- */
#define LOG_REFRESH_MS        200     // be kind to LVGL
#define LOG_UI_MAX_CHARS      3000    // keep UI text bounded
#define LOG_DRAIN_CHUNK       512     // max read chunk from ringbuffer
#define LOG_APPEND_MAX        1024    // max appended to textarea per tick (IMPORTANT)
#define LOG_TRIM_EVERY_N_TICKS 8      // trim about every ~1.6s at 200ms refresh

/* ---------------- Helpers ---------------- */
static bool log_user_is_at_bottom(lv_obj_t *ta)
{
    // LVGL8 textarea internal label/cont is typically child(0)
    lv_obj_t *cont = lv_obj_get_child(ta, 0);
    if (!cont) return true;

    lv_coord_t y  = lv_obj_get_y(cont);
    lv_coord_t ch = lv_obj_get_height(cont);
    lv_coord_t vh = lv_obj_get_height(ta);

    lv_coord_t bottom_y = -(ch - vh);
    return (y <= bottom_y + 20);
}

static void trim_textarea_to_last(lv_obj_t *ta, size_t keep_last)
{
    const char *cur = lv_textarea_get_text(ta);
    if (!cur) return;

    size_t n = strlen(cur);
    if (n <= keep_last) return;

    size_t start = n - keep_last;

    // move start to next newline so we don't start mid-line
    while (start < n && cur[start] != '\n') start++;
    if (start < n) start++;

    // Expensive: forces full relayout. Use rarely.
    lv_textarea_set_text(ta, cur + start);
}

/* sanitize: keep printable ASCII, tab, newline. normalize CR -> LF */
static inline bool log_char_ok(char *c_inout)
{
    char c = *c_inout;
    if (c == '\r') c = '\n';

    unsigned char uc = (unsigned char)c;
    if (c == '\n' || c == '\t') { *c_inout = c; return true; }
    if (uc >= 32 && uc <= 126)  { *c_inout = c; return true; }
    return false;
}

/* ---------------- Timer callback ---------------- */
static void log_screen_refresh_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_log_ta) return;

    static char chunk[LOG_DRAIN_CHUNK];
    static char append[LOG_APPEND_MAX + 1];
    static uint32_t tick_ctr = 0;

    bool at_bottom = log_user_is_at_bottom(s_log_ta);

    size_t app_len = 0;
    bool appended_any = false;

    // Drain a couple reads per tick to keep LVGL responsive
    for (int i = 0; i < 2 && app_len < LOG_APPEND_MAX; i++) {

        size_t got = watch_logstream_read(chunk, sizeof(chunk));
        if (!got) break;

        for (size_t k = 0; k < got; k++) {
            char c = chunk[k];
            if (!log_char_ok(&c)) continue;

            // --- build line in s_line_acc safely ---
            if (s_line_acc_len < (sizeof(s_line_acc) - 1)) {
                s_line_acc[s_line_acc_len++] = c;
            } else {
                // Line buffer full: force flush as a complete line
                // Ensure it ends with '\n' if possible
                if (s_line_acc_len > 0 && s_line_acc[s_line_acc_len - 1] != '\n') {
                    // replace last char with newline (no overflow)
                    s_line_acc[s_line_acc_len - 1] = '\n';
                }
                c = '\n'; // treat as newline for flush logic below
            }

            // Flush on newline (or forced)
            if (c == '\n') {
                // NUL terminate safely
                s_line_acc[s_line_acc_len] = '\0';

                size_t need = s_line_acc_len;
                if (need > (LOG_APPEND_MAX - app_len)) {
                    // Not enough room this tick: keep the line for next tick
                    // (do nothing; leave s_line_acc_len as-is)
                    goto done_drain;
                }

                memcpy(append + app_len, s_line_acc, need);
                app_len += need;

                // reset accumulator
                s_line_acc_len = 0;
            }
        }
    }

done_drain:
    if (app_len) {
        append[app_len] = '\0';
        lv_textarea_add_text(s_log_ta, append);   // one call per tick
        appended_any = true;
    }

    // Trim occasionally (expensive)
    if (++tick_ctr >= LOG_TRIM_EVERY_N_TICKS) {
        tick_ctr = 0;
        trim_textarea_to_last(s_log_ta, LOG_UI_MAX_CHARS);
    }

    if (at_bottom && appended_any) {
        lv_textarea_set_cursor_pos(s_log_ta, LV_TEXTAREA_CURSOR_LAST);
    }
}

/* ---------------- Events ---------------- */
static void on_log_back(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_show(UI_HOME);
}

static void on_log_screen_delete(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;

    s_log_ta = NULL;
    s_line_acc_len = 0;

    if (s_log_timer) lv_timer_pause(s_log_timer);
}

/* public (used by ui_show) */
void ui_log_screen_pause(bool pause)
{
    if (!s_log_timer) return;
    if (pause) lv_timer_pause(s_log_timer);
    else lv_timer_resume(s_log_timer);

}

/* ---------------- Build screen ---------------- */
lv_obj_t *ui_build_log_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_add_event_cb(scr, on_log_screen_delete, LV_EVENT_DELETE, NULL);

    /* back button */
    lv_obj_t *back = lv_btn_create(scr);
    lv_obj_set_size(back, 25, 25);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_60, 0);
    lv_obj_add_event_cb(back, on_log_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btxt = lv_label_create(back);
    lv_label_set_text(btxt, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(btxt, lv_color_hex(0xfc0303), 0);
    lv_obj_center(btxt);

    /* textarea */
    s_log_ta = lv_textarea_create(scr);
    lv_obj_set_size(s_log_ta, lv_pct(100), lv_pct(90));
    lv_obj_align(s_log_ta, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_top(s_log_ta, 60, 0);

    lv_textarea_set_one_line(s_log_ta, false);
    lv_textarea_set_cursor_click_pos(s_log_ta, false);
    lv_textarea_set_text_selection(s_log_ta, false);
    lv_obj_clear_flag(s_log_ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    lv_obj_set_style_text_color(s_log_ta, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_log_ta, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_log_ta, LV_OPA_COVER, 0);

    /* reset state for clean open */
    s_line_acc_len = 0;

    /* timer */
    if (!s_log_timer) {
        s_log_timer = lv_timer_create(log_screen_refresh_cb, LOG_REFRESH_MS, NULL);
    } else {
        lv_timer_set_period(s_log_timer, LOG_REFRESH_MS);
    }
    lv_timer_resume(s_log_timer);

    // IMPORTANT: do NOT force an immediate drain here (can WDT on open)
    return scr;
}
