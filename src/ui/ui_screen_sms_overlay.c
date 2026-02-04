// FILE: src/ui/ui_screen_sms_overlay.c
#include "ui_priv.h"
#include "ui_color_pallete.h"
#include "watch_sms_store.h"
#include "watch_time.h"    // if you have time formatting helpers; optional
#include "esp_log.h"
#include <stdio.h>
#include <inttypes.h>

static const char *TAG = "UI_SMS_OVL";

static lv_obj_t *s_sender = NULL;
static lv_obj_t *s_body   = NULL;
static lv_obj_t *s_time   = NULL;
static void stop_bubble(lv_event_t *e) { lv_event_stop_bubbling(e); }

static void overlay_close(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_show(UI_CLOCK); // go back to clock (or ui_pop() if you have stack)
}

static void overlay_open_threads(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_show(UI_SMS_THREADS);
}

static void overlay_refresh(void)
{
    watch_sms_latest_t m;
    if (!watch_sms_get_latest(&m)) {
        if (s_sender) lv_label_set_text(s_sender, "No messages yet");
        if (s_body)   lv_label_set_text(s_body, "");
        if (s_time)   lv_label_set_text(s_time, "");
        return;
    }

    if (s_sender) lv_label_set_text(s_sender, m.sender);

    // Body can be long; LVGL wrap
    if (s_body) lv_label_set_text(s_body, m.body);

    // Optional: show timestamp ms as something basic
    if (s_time) {
        char b[48];
        // If you don’t have a formatter yet, just show ms or omit.
        snprintf(b, sizeof(b), "ts=%" PRIu64, (uint64_t)m.ts_ms);
        lv_label_set_text(s_time, b);
    }
}

lv_obj_t *ui_build_sms_overlay_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_opa(scr, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Fullscreen click closes overlay (tap outside card)
    lv_obj_add_event_cb(scr, overlay_close, LV_EVENT_CLICKED, NULL);

    // Card container
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, lv_pct(92), lv_pct(55));
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x151515), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, UI_COLOR(THEME), 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Stop clicks on card from closing the overlay
    lv_obj_add_event_cb(card, stop_bubble, LV_EVENT_CLICKED, NULL);

    // Title row
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Latest message");
    lv_obj_set_style_text_color(title, UI_COLOR(THEME), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    // Sender
    s_sender = lv_label_create(card);
    lv_obj_set_style_text_color(s_sender, UI_COLOR(WHITE), 0);
    lv_obj_set_style_text_font(s_sender, &lv_font_montserrat_20, 0);
    lv_obj_align_to(s_sender, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);
    lv_label_set_long_mode(s_sender, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_sender, lv_pct(100));

    // Time
    s_time = lv_label_create(card);
    lv_obj_set_style_text_color(s_time, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_14, 0);
    lv_obj_align_to(s_time, s_sender, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);

    // Body scroller
    lv_obj_t *body_sc = lv_obj_create(card);
    lv_obj_set_width(body_sc, lv_pct(100));
    lv_obj_set_height(body_sc, lv_pct(52));                 // tune if you want more/less
    lv_obj_align_to(body_sc, s_time, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);

    lv_obj_set_style_bg_opa(body_sc, LV_OPA_0, 0);
    lv_obj_set_style_border_width(body_sc, 0, 0);
    lv_obj_set_style_pad_all(body_sc, 0, 0);

    lv_obj_set_scroll_dir(body_sc, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body_sc, LV_SCROLLBAR_MODE_AUTO);

    // Body label (wrap)
    s_body = lv_label_create(body_sc);
    lv_obj_set_style_text_color(s_body, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_style_text_font(s_body, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_body, lv_pct(100));
    lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 0, 0);


    // Buttons row
    lv_obj_t *btn_row = lv_obj_create(card);
    lv_obj_set_size(btn_row, lv_pct(100), 46);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_0, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *open = lv_btn_create(btn_row);
    lv_obj_set_size(open, lv_pct(48), 44);
    lv_obj_align(open, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(open, overlay_open_threads, LV_EVENT_CLICKED, NULL);

    lv_obj_t *open_lbl = lv_label_create(open);
    lv_label_set_text(open_lbl, "Open");
    lv_obj_center(open_lbl);

    lv_obj_t *close = lv_btn_create(btn_row);
    lv_obj_set_size(close, lv_pct(48), 44);
    lv_obj_align(close, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(close, overlay_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close_lbl = lv_label_create(close);
    lv_label_set_text(close_lbl, "Close");
    lv_obj_center(close_lbl);

    overlay_refresh();
    return scr;
}
