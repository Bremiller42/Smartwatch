// FILE: src/ui/ui_screen_sms_thread_view.c
#include "ui_priv.h"
#include "ui_color_pallete.h"
#include "watch_sdcard.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "UI_SMS_THREAD";

#define SMS_LOG_PATH   "/sdcard/sms/messages.log"
#define MAX_LOG_BYTES  (64 * 1024)
#define MAX_MSGS       48
#define THREAD_ID_SZ   192

typedef struct {
    uint64_t ts_ms;
    char sender[96];
    char body[256]; // render preview; you can increase later
} sms_msg_t;

static sms_msg_t s_msgs[MAX_MSGS];
static int s_msg_count = 0;

static lv_obj_t *s_col = NULL;

static void msgs_clear(void)
{
    memset(s_msgs, 0, sizeof(s_msgs));
    s_msg_count = 0;
}

static void strlcpy0(char *dst, size_t dstsz, const char *src)
{
    if (!dst || dstsz == 0) return;
    dst[0] = 0;
    if (!src) return;
    size_t n = strlen(src);
    if (n >= dstsz) n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void body_sanitize(char *dst, size_t dstsz, const char *src)
{
    if (!dst || dstsz == 0) return;
    dst[0] = 0;
    if (!src) return;

    size_t w = 0;
    for (size_t i = 0; src[i] && w < dstsz - 1; i++) {
        char c = src[i];
        if (c == '\r') continue;
        // keep '\n' for display wrap? you can keep it or convert to space:
        if (c == '\n') c = '\n';
        if (c == '|') c = ' ';
        dst[w++] = c;
    }
    dst[w] = 0;
}

static void ingest_line_for_thread(char *line, const char *thread_id)
{
    // msg_id|ts_ms|thread_id|sender|pkg|body
    char *save = NULL;
    (void)strtok_r(line, "|", &save);
    char *ts   = strtok_r(NULL, "|", &save);
    char *tid  = strtok_r(NULL, "|", &save);
    char *snd  = strtok_r(NULL, "|", &save);
    (void)strtok_r(NULL, "|", &save);
    char *body = strtok_r(NULL, "|", &save);

    if (!ts || !tid || !snd || !body) return;
    if (strcmp(tid, thread_id) != 0) return;

    // keep last MAX_MSGS messages by shifting when full
    if (s_msg_count >= MAX_MSGS) {
        memmove(&s_msgs[0], &s_msgs[1], sizeof(sms_msg_t) * (MAX_MSGS - 1));
        s_msg_count = MAX_MSGS - 1;
    }

    sms_msg_t *m = &s_msgs[s_msg_count++];
    m->ts_ms = strtoull(ts, NULL, 10);
    strlcpy0(m->sender, sizeof(m->sender), snd);
    body_sanitize(m->body, sizeof(m->body), body);
}

typedef struct { uint8_t *buf; size_t len; } load_ctx_t;
static esp_err_t load_log_work(void *p)
{
    load_ctx_t *c = (load_ctx_t*)p;
    return watch_sdcard_read_file(SMS_LOG_PATH, &c->buf, &c->len, MAX_LOG_BYTES, 0);
}

static void load_thread_msgs(const char *thread_id)
{
    msgs_clear();

    uint8_t *buf = NULL;
    size_t   len = 0;

    esp_err_t e = watch_sdcard_read_file(SMS_LOG_PATH, &buf, &len, MAX_LOG_BYTES, 5000);
    if (e != ESP_OK || !buf || len == 0) {
        if (buf) free(buf);
        return;
    }

    char *s = (char*)buf;
    char *line = s;

    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n') {
            s[i] = 0;
            if (line[0]) {
                size_t L = strlen(line);
                if (L && line[L - 1] == '\r') line[L - 1] = 0;
                ingest_line_for_thread(line, thread_id);
            }
            line = &s[i + 1];
        }
    }
    if (line && *line) ingest_line_for_thread(line, thread_id);

    free(buf);
}


static void on_back(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_show(UI_SMS_THREADS);
}

static void render_msgs(void)
{
    if (!s_col) return;
    lv_obj_clean(s_col);

    if (s_msg_count == 0) {
        lv_obj_t *lbl = lv_label_create(s_col);
        lv_label_set_text(lbl, "No messages in this thread.");
        return;
    }

    for (int i = 0; i < s_msg_count; i++) {
        sms_msg_t *m = &s_msgs[i];

        lv_obj_t *bubble = lv_obj_create(s_col);
        lv_obj_set_width(bubble, lv_pct(100));
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bubble, 14, 0);
        lv_obj_set_style_pad_all(bubble, 10, 0);
        lv_obj_set_style_border_width(bubble, 1, 0);
        lv_obj_set_style_border_color(bubble, lv_color_hex(0x333333), 0);
        lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *hdr = lv_label_create(bubble);
        lv_label_set_text(hdr, m->sender);
        lv_obj_set_style_text_color(hdr, UI_COLOR(THEME), 0);
        lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);

        lv_obj_t *body = lv_label_create(bubble);
        lv_label_set_text(body, m->body);
        lv_obj_set_style_text_color(body, lv_color_hex(0xDDDDDD), 0);
        lv_obj_set_style_text_font(body, &lv_font_montserrat_16, 0);
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(body, lv_pct(100));
        lv_obj_align_to(body, hdr, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);

        lv_obj_t *sp = lv_obj_create(s_col);
        lv_obj_set_size(sp, 1, 10);
        lv_obj_set_style_bg_opa(sp, LV_OPA_0, 0);
        lv_obj_set_style_border_width(sp, 0, 0);
    }
}

lv_obj_t *ui_build_sms_thread_view_screen(void)
{
    const char *tid = ui_sms_get_active_thread_id();
    const char *name = ui_sms_get_active_thread_name();

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_btn_create(scr);
    lv_obj_set_size(back, 84, 40);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b = lv_label_create(back);
    lv_label_set_text(b, LV_SYMBOL_LEFT);
    lv_obj_center(b);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, (name && *name) ? name : "Conversation");
    lv_obj_set_style_text_color(title, UI_COLOR(THEME), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, lv_pct(70));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *sc = lv_obj_create(scr);
    lv_obj_set_size(sc, lv_pct(94), lv_pct(80));
    lv_obj_align(sc, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_opa(sc, LV_OPA_0, 0);
    lv_obj_set_style_border_width(sc, 0, 0);
    lv_obj_set_style_pad_all(sc, 0, 0);

    // Make container scrollable
    lv_obj_set_scroll_dir(sc, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_AUTO);

    s_col = lv_obj_create(sc);
    lv_obj_set_width(s_col, lv_pct(100));
    lv_obj_set_style_bg_opa(s_col, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_col, 0, 0);
    lv_obj_set_style_pad_all(s_col, 0, 0);
    lv_obj_set_flex_flow(s_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(s_col, LV_OBJ_FLAG_SCROLLABLE);

    if (tid && *tid) load_thread_msgs(tid);
    render_msgs();

    // scroll to bottom
    lv_obj_scroll_to_y(sc, lv_obj_get_height(s_col), LV_ANIM_OFF);

    return scr;
}
