// FILE: src/ui/ui_modal_wifi.c
#include "ui_priv.h"
#include "esp_log.h"
#include "watch_wifi.h"

static const char *UI_WIFI_TAG = "UI_WIFI";

static void on_wifi_picker_close(lv_event_t *e);
static void on_wifi_picker_scan(lv_event_t *e);
static void on_wifi_forget_clicked(lv_event_t *e);

void on_wifi_ap_clicked(lv_event_t *e);     // used by wifi module
void on_wifi_pass_cancel(lv_event_t *e);
void on_wifi_pass_connect(lv_event_t *e);

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

    lv_obj_t *btn_frgt = lv_btn_create(card);
    lv_obj_set_size(btn_frgt, 90, 32);
    lv_obj_align(btn_frgt, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(btn_frgt, on_wifi_forget_clicked, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(btn_frgt), "Forget");
    lv_obj_center(lv_obj_get_child(btn_frgt, 0));

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

    ESP_LOGI(UI_WIFI_TAG, "Connecting to selected SSID...");
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

static void on_wifi_forget_clicked(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    wifi_forget_saved();
    lv_async_call(ui_update_wifi_icon_async, NULL);
    start_wifi_scan();
}

/* Export this if settings screen uses it directly */
void ui_open_wifi_picker_from_settings(lv_event_t *e)
{
    on_open_wifi_picker(e);
}
