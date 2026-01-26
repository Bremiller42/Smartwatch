// FILE: src/ui/ui_screen_log.c
// Fast + stable log viewer (LVGL8)
//
// Implements:
// 1) seq-based early exit (no work when no new logs)
// 2) label in scrollable container (faster than textarea)
// 3) partial lines show immediately (no newline required)
// 4) bounded drain per tick; never stalls on long lines
// 5) UI-side ring buffer (no expensive widget trimming)
// 6) robust autoscroll: follows only if user is at bottom
// 7) "Bottom" button appears when user scrolls up

#include "lvgl.h"
#include "ui_priv.h"
#include "watch_logstream.h"
#include "watch_logbuf.h"
#include "watch_screen_timeout.h"

#include "ui_color_pallete.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ---------------- Tune ---------------- */
#define LOG_REFRESH_MS        120     // refresh period
#define LOG_DRAIN_CHUNK       512     // read chunk size from stream
#define LOG_APPEND_MAX        768     // max bytes appended per tick (bounded work)
#define LOG_UI_RING_CHARS     4096    // total chars kept in UI buffer
#define LOG_SCROLL_THRESH_PX  24      // at-bottom threshold
#define LOG_BOTTOM_BTN_H      34
#define LOG_BOTTOM_BTN_PAD    8
#define LOG_BOTTOM_SAFE_PAD   (LOG_BOTTOM_BTN_H + (LOG_BOTTOM_BTN_PAD * 2))

/* ---------------- UI objects ---------------- */
static lv_obj_t   *s_cont          = NULL;   // scrollable container
static lv_obj_t   *s_label         = NULL;   // label holding log text
static lv_timer_t *s_timer         = NULL;
static lv_obj_t   *s_btn_bottom    = NULL;
static lv_obj_t   *s_btn_bottom_lbl= NULL;

/* ---------------- Autoscroll state ---------------- */
static bool s_autoscroll      = true;   // follow tail by default
static bool s_user_scrolling  = false;  // true while finger is down / scroll in progress

/* ---------------- UI-side ring buffer ---------------- */
static char   s_ring[LOG_UI_RING_CHARS];
static size_t s_ring_head     = 0;
static bool   s_ring_wrapped  = false;

/* contiguous view buffer for label */
static char s_view[LOG_UI_RING_CHARS + 1];

/* last observed seq from stream */
static uint32_t s_last_seq = 0;

/* ---------------- Forward decls ---------------- */
static bool   cont_is_at_bottom(void);
static void   cont_scroll_to_bottom(void);
static void   bottom_btn_update_visible(void);
static void   scroll_to_bottom_oneshot(lv_timer_t *t);

/* ---------------- Helpers ---------------- */

static inline bool log_char_ok(char *c_inout)
{
    char c = *c_inout;
    if (c == '\r') c = '\n';

    unsigned char uc = (unsigned char)c;
    if (c == '\n' || c == '\t') { *c_inout = c; return true; }
    if (uc >= 32 && uc <= 126)  { *c_inout = c; return true; }
    return false;
}

static void ring_reset(void)
{
    s_ring_head = 0;
    s_ring_wrapped = false;
    memset(s_ring, 0, sizeof(s_ring));
}

static inline void ring_put_char(char c)
{
    s_ring[s_ring_head++] = c;
    if (s_ring_head >= LOG_UI_RING_CHARS) {
        s_ring_head = 0;
        s_ring_wrapped = true;
    }
}

static size_t log_drain_once(size_t max_bytes)
{
    static char chunk[LOG_DRAIN_CHUNK];
    size_t appended = 0;

    /* bounded reads per tick so we don't stall */
    for (int reads = 0; reads < 4 && appended < max_bytes; reads++) {
        size_t got = watch_logstream_read(chunk, sizeof(chunk));
        if (!got) break;

        for (size_t i = 0; i < got && appended < max_bytes; i++) {
            char c = chunk[i];
            if (!log_char_ok(&c)) continue;
            ring_put_char(c);
            appended++;
        }
    }
    return appended;
}

/* Build a contiguous view into s_view, newline-aligned at start if wrapped */
static const char *ring_build_view(void)
{
    size_t n = 0;

    if (!s_ring_wrapped) {
        n = s_ring_head;
        memcpy(s_view, s_ring, n);
        s_view[n] = '\0';
        return s_view;
    }

    /* wrapped: oldest begins at head */
    size_t first  = LOG_UI_RING_CHARS - s_ring_head;
    size_t second = s_ring_head;

    memcpy(s_view, &s_ring[s_ring_head], first);
    memcpy(s_view + first, s_ring, second);
    n = first + second;
    s_view[n] = '\0';

    /* Align start to next newline so we don't start mid-line */
    size_t start = 0;
    while (start < n && s_view[start] != '\n') start++;
    if (start < n) start++;

    return (start < n) ? (s_view + start) : s_view;
}

static bool cont_is_at_bottom(void)
{
    if (!s_cont) return true;
    /* LVGL8: returns distance remaining to bottom. 0 means at bottom. */
    return (lv_obj_get_scroll_bottom(s_cont) <= LOG_SCROLL_THRESH_PX);
}

/* Always scroll container to its bottom-most scroll position.
   IMPORTANT: lv_obj_get_scroll_bottom() is a distance; the bottom offset is -distance (top is 0). */
static void cont_scroll_to_bottom(void)
{
    if (!s_cont) return;

    /* Ensure scroll range is up to date */
    lv_obj_update_layout(s_cont);

    lv_coord_t bottom_dist = lv_obj_get_scroll_bottom(s_cont);
    lv_obj_scroll_to_y(s_cont, bottom_dist, LV_ANIM_OFF);
}

static void bottom_btn_update_visible(void)
{
    if (!s_btn_bottom) return;

    if (cont_is_at_bottom()) {
        lv_obj_add_flag(s_btn_bottom, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_btn_bottom, LV_OBJ_FLAG_HIDDEN);
    }
}

/* One-shot that runs after screen is built so layout/scroll range is valid */
static void scroll_to_bottom_oneshot(lv_timer_t *t)
{
    (void)t;

    /* Some ports need one extra pass for layout to settle.
       Two calls are cheap and makes this rock-solid. */
    cont_scroll_to_bottom();
    cont_scroll_to_bottom();

    s_autoscroll = true;
    s_user_scrolling = false;

    bottom_btn_update_visible();
    lv_timer_del(t);
}

/* ---------------- Events ---------------- */

static void on_log_back(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_show(UI_HOME);
}

static void on_bottom_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    /* Re-enable follow mode and jump to tail */
    s_user_scrolling = false;
    s_autoscroll = true;

    cont_scroll_to_bottom();
    bottom_btn_update_visible();
}

static void on_cont_scroll(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_SCROLL_BEGIN) {
        s_user_scrolling = true;
        /* don't change s_autoscroll yet; user might still be at bottom */
        bottom_btn_update_visible();
        return;
    }

    if (code == LV_EVENT_SCROLL_END) {
        s_user_scrolling = false;
        /* follow only if they ended at bottom */
        s_autoscroll = cont_is_at_bottom();
        bottom_btn_update_visible();
        return;
    }

    if (code == LV_EVENT_SCROLL) {
        /* while moving, disable follow if they leave bottom */
        if (!cont_is_at_bottom()) s_autoscroll = false;
        else s_autoscroll = true;

        bottom_btn_update_visible();
    }
}
static int s_log_to_token = -1;   // move this to file-scope, not inside ui_build_log_screen

static void on_log_screen_delete(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;

    if (s_log_to_token >= 0) {
        screen_timeout_pop_override(s_log_to_token);
        s_log_to_token = -1;
        screen_timeout_mark_activity(); // recompute deadline with default timeout
    }

    s_cont = NULL;
    s_label = NULL;
    s_btn_bottom = NULL;
    s_btn_bottom_lbl = NULL;

    if (s_timer) lv_timer_pause(s_timer);
}

/* ---------------- Timer callback ---------------- */

static void log_refresh_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_label || !s_cont) return;

    /* Follow tail only if user is at bottom (or returns to bottom) */
    bool at_bottom_now = cont_is_at_bottom();
    if (!s_user_scrolling) {
        if (!at_bottom_now) s_autoscroll = false;
        else s_autoscroll = true;
    }

    /* Gate by stream seq */
    uint32_t seq = watch_logstream_seq();
    if (seq == s_last_seq) {
        /* still keep bottom button honest while user scrolls */
        bottom_btn_update_visible();
        return;
    }

    size_t appended = log_drain_once(LOG_APPEND_MAX);
    s_last_seq = seq;

    if (!appended) {
        bottom_btn_update_visible();
        return;
    }

    /* Render once (static pointer into s_view; we rebuild on each tick) */
    const char *view = ring_build_view();
    lv_label_set_text_static(s_label, view);
    lv_obj_set_width(s_label, lv_obj_get_width(s_cont));

    if (s_autoscroll && !s_user_scrolling) {
        cont_scroll_to_bottom();
    }

    bottom_btn_update_visible();
}

/* public (used by ui_show) */
void ui_log_screen_pause(bool pause)
{
    if (!s_timer) return;
    if (pause) lv_timer_pause(s_timer);
    else       lv_timer_resume(s_timer);
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
    lv_obj_set_style_text_color(btxt, UI_COLOR(RED), 0);
    lv_obj_center(btxt);

    /* scrollable container */
    s_cont = lv_obj_create(scr);
    lv_obj_set_size(s_cont, lv_pct(100), lv_pct(90));
    lv_obj_align(s_cont, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_cont, 10, 0);
    lv_obj_set_style_pad_top(s_cont, 50, 0);

    /* Leave room so overlay button doesn't cover last lines */
    lv_obj_set_style_pad_bottom(s_cont, LOG_BOTTOM_SAFE_PAD, 0);

    lv_obj_set_scroll_dir(s_cont, LV_DIR_VER);
    lv_obj_add_flag(s_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_cont, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);

    lv_obj_add_event_cb(s_cont, on_cont_scroll, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(s_cont, on_cont_scroll, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(s_cont, on_cont_scroll, LV_EVENT_SCROLL_END, NULL);

    /* log label */
    s_label = lv_label_create(s_cont);
    lv_obj_set_width(s_label, lv_obj_get_width(s_cont));
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_label, UI_COLOR(THEME), 0);
    lv_obj_set_style_text_font(s_label, LV_FONT_DEFAULT, 0);

    /* Bottom button (overlay) */
    s_btn_bottom = lv_btn_create(scr);
    lv_obj_set_size(s_btn_bottom, 120, LOG_BOTTOM_BTN_H);
    lv_obj_align(s_btn_bottom, LV_ALIGN_BOTTOM_MID, 0, -LOG_BOTTOM_BTN_PAD);
    lv_obj_set_style_radius(s_btn_bottom, 10, 0);
    lv_obj_set_style_bg_color(s_btn_bottom, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(s_btn_bottom, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_btn_bottom, 1, 0);
    lv_obj_set_style_border_color(s_btn_bottom, lv_color_hex(0x404040), 0);
    lv_obj_add_event_cb(s_btn_bottom, on_bottom_btn, LV_EVENT_CLICKED, NULL);

    s_btn_bottom_lbl = lv_label_create(s_btn_bottom);
    lv_label_set_text(s_btn_bottom_lbl, LV_SYMBOL_DOWN " Bottom");
    lv_obj_set_style_text_color(s_btn_bottom_lbl, UI_COLOR(THEME), 0);
    lv_obj_center(s_btn_bottom_lbl);

    /* start hidden (we will show it if user scrolls up) */
    lv_obj_add_flag(s_btn_bottom, LV_OBJ_FLAG_HIDDEN);

    /* reset viewer state */
    ring_reset();
    s_autoscroll = true;
    s_user_scrolling = false;
    s_last_seq = 0;

    /* timer */
    if (!s_timer) {
        s_timer = lv_timer_create(log_refresh_cb, LOG_REFRESH_MS, NULL);
    } else {
        lv_timer_set_period(s_timer, LOG_REFRESH_MS);
    }

    /* ---- Initial population (bounded, non-blocking) ---- */
    size_t appended = 0;
    for (int i = 0; i < 6; i++) {
        appended += log_drain_once(LOG_APPEND_MAX);
        /* probe: if nothing left right now, stop */
        if (watch_logstream_read((char[2]){0}, 2) == 0) break;
    }

    if (appended) {
        const char *view = ring_build_view();
        lv_label_set_text_static(s_label, view);
        lv_obj_set_width(s_label, lv_obj_get_width(s_cont));
    }

    /* Start ticking + force scroll to bottom after layout settles */
    s_last_seq = watch_logstream_seq();
    lv_timer_resume(s_timer);

    lv_timer_create(scroll_to_bottom_oneshot, 1, NULL);
    s_log_to_token = screen_timeout_push_override_ms(0); // always on
    screen_timeout_mark_activity();

    return scr;
}
