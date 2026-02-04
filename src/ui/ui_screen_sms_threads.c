// FILE: src/ui/ui_screen_sms_threads.c
#include "ui_priv.h"
#include "ui_color_pallete.h"
#include "watch_sdcard.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>

static const char *TAG = "UI_SMS_THREADS";

#define SMS_LOG_PATH   "/sdcard/sms/messages.log"
#define MAX_LOG_BYTES  (64 * 1024)

#define MAX_THREADS    32
#define THREAD_ID_SZ   192
#define SENDER_SZ      96
#define SNIP_SZ        96

typedef struct {
    bool used;
    uint64_t ts_ms;
    char thread_id[THREAD_ID_SZ];
    char sender[SENDER_SZ];
    char snippet[SNIP_SZ];
} sms_thread_item_t;

static sms_thread_item_t s_threads[MAX_THREADS];

static lv_obj_t *s_list = NULL;

static void threads_clear(void) { memset(s_threads, 0, sizeof(s_threads)); }

static int threads_find_by_sender(const char *sender)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        if (s_threads[i].used && strcmp(s_threads[i].sender, sender) == 0) return i;
    }
    return -1;
}

static int threads_free_slot(void)
{
    for (int i = 0; i < MAX_THREADS; i++) if (!s_threads[i].used) return i;
    return -1;
}

static void snip_copy(char *dst, size_t dstsz, const char *src)
{
    if (!dst || dstsz == 0) return;
    dst[0] = 0;
    if (!src) return;

    size_t n = strlen(src);
    if (n >= dstsz) n = dstsz - 1;

    // squash newlines
    size_t w = 0;
    for (size_t i = 0; i < n && w < dstsz - 1; i++) {
        char c = src[i];
        if (c == '\r' || c == '\n') c = ' ';
        dst[w++] = c;
    }
    dst[w] = 0;
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

static void ingest_line(char *line)
{
    // Format: msg_id|ts_ms|thread_id|sender|pkg|body
    // We only need ts, thread_id, sender, body(snippet)
    // Tokenize in-place.
    char *save = NULL;

    (void)strtok_r(line, "|", &save);            // msg_id
    char *ts   = strtok_r(NULL, "|", &save);     // ts_ms
    char *tid  = strtok_r(NULL, "|", &save);     // thread_id
    char *snd  = strtok_r(NULL, "|", &save);     // sender
    (void)strtok_r(NULL, "|", &save);            // pkg
    char *body = strtok_r(NULL, "|", &save);     // body

    if (!ts || !tid || !snd || !body) return;

    uint64_t tms = (uint64_t)strtoull(ts, NULL, 10);

    int idx = threads_find_by_sender(snd);
    if (idx < 0) idx = threads_free_slot();
    if (idx < 0) return;

    // Keep the most recent record per thread
    if (s_threads[idx].used && tms <= s_threads[idx].ts_ms) return;

    s_threads[idx].used = true;
    s_threads[idx].ts_ms = tms;
    strlcpy0(s_threads[idx].thread_id, sizeof(s_threads[idx].thread_id), tid);
    strlcpy0(s_threads[idx].sender, sizeof(s_threads[idx].sender), snd);
    snip_copy(s_threads[idx].snippet, sizeof(s_threads[idx].snippet), body);
}

static int thread_sort_cmp(const void *a, const void *b)
{
    const sms_thread_item_t *A = (const sms_thread_item_t*)a;
    const sms_thread_item_t *B = (const sms_thread_item_t*)b;

    if (!A->used && !B->used) return 0;
    if (!A->used) return 1;
    if (!B->used) return -1;

    if (A->ts_ms > B->ts_ms) return -1;
    if (A->ts_ms < B->ts_ms) return 1;
    return 0;
}

typedef struct {
    uint8_t *buf;
    size_t len;
} load_ctx_t;

static esp_err_t load_log_work(void *p)
{
    load_ctx_t *c = (load_ctx_t*)p;
    return watch_sdcard_read_file(SMS_LOG_PATH, &c->buf, &c->len, MAX_LOG_BYTES, 0);
}

static void threads_load_from_sd(void)
{
    threads_clear();

    uint8_t *buf = NULL;
    size_t   len = 0;

    esp_err_t e = watch_sdcard_read_file(SMS_LOG_PATH, &buf, &len, MAX_LOG_BYTES, 5000);
    if (e != ESP_OK || !buf || len == 0) {
        ESP_LOGW(TAG, "No log or read failed: %s", esp_err_to_name(e));
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
                ingest_line(line);
            }
            line = &s[i + 1];
        }
    }
    if (line && *line) ingest_line(line);

    free(buf);

    qsort(s_threads, MAX_THREADS, sizeof(sms_thread_item_t), thread_sort_cmp);
}

static void on_back(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_show(UI_CLOCK);
}

static void on_thread_click(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    sms_thread_item_t *it = (sms_thread_item_t*)lv_event_get_user_data(e);
    if (!it || !it->used) return;

    // Store selected thread_id somewhere global for the convo screen
    ui_sms_set_active_thread(it->thread_id, it->sender); // we’ll add this in ui_priv.h/.c
    ui_show(UI_SMS_THREAD_VIEW);
}

static void threads_render(void)
{
    if (!s_list) return;
    lv_obj_clean(s_list);

    bool any = false;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!s_threads[i].used) continue;
        any = true;

        char title[160];
        snprintf(title, sizeof(title), "%s", s_threads[i].sender);

        lv_obj_t *btn = lv_list_add_btn(s_list, LV_SYMBOL_CALL, title);
        lv_obj_add_event_cb(btn, on_thread_click, LV_EVENT_CLICKED, &s_threads[i]);

        // small snippet label under the button (LVGL list item is a button; add a label child)
        lv_obj_t *sn = lv_label_create(btn);
        lv_label_set_text(sn, s_threads[i].snippet);
        lv_obj_set_style_text_color(sn, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_text_font(sn, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(sn, LV_LABEL_LONG_DOT);
        lv_obj_set_width(sn, lv_pct(85));
        lv_obj_align(sn, LV_ALIGN_BOTTOM_LEFT, 42, -2);
    }

    if (!any) {
        lv_obj_t *lbl = lv_label_create(s_list);
        lv_label_set_text(lbl, "No SMS yet.\n(Need messages.log)");
        lv_obj_center(lbl);
    }
}

lv_obj_t *ui_build_sms_threads_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SMS Threads");
    lv_obj_set_style_text_color(title, UI_COLOR(THEME), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *back = lv_btn_create(scr);
    lv_obj_set_size(back, 84, 40);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b = lv_label_create(back);
    lv_label_set_text(b, LV_SYMBOL_LEFT);
    lv_obj_center(b);

    s_list = lv_list_create(scr);
    lv_obj_set_size(s_list, lv_pct(94), lv_pct(78));
    lv_obj_align(s_list, LV_ALIGN_BOTTOM_MID, 0, -10);

    threads_load_from_sd();
    threads_render();
    return scr;
}
