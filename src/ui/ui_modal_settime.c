// FILE: src/ui/ui_modal_settime.c
#include "ui_priv.h"
#include "esp_log.h"
#include <time.h>

static const char *UI_ST_TAG = "UI_STTIME";

static void settime_close(void);
static void settime_refresh_labels(void);

static void on_settime_cancel(lv_event_t * e);
static void on_settime_save(lv_event_t * e);
static void on_inc_hour(lv_event_t *e);
static void on_dec_hour(lv_event_t *e);
static void on_inc_min(lv_event_t *e);
static void on_dec_min(lv_event_t *e);

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

void open_settime_modal(lv_obj_t *parent)
{
    if (settime_modal) return;
    ESP_LOGI(UI_ST_TAG, "Opening Settime");

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
