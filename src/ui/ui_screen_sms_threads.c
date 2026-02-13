// FILE: src/ui/ui_screen_sms_threads.c
#include "ui_priv.h"
#include "ui_color_pallete.h"
#include "watch_sdcard.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include "watch_ble.h"   // for notif_type_t + mapping

static const char *TAG = "UI_SMS_THREADS";

#define SMS_LOG_PATH   "/sdcard/sms/messages.log"
#define MAX_LOG_BYTES  (64 * 1024)

#define MAX_THREADS    32
#define THREAD_ID_SZ   192
#define SENDER_SZ      96
#define SNIP_SZ        96
extern const lv_img_dsc_t *icon_for_group(notif_type_t g);

typedef struct {
    bool used;
    uint64_t ts_ms;
    char thread_id[THREAD_ID_SZ];
    char sender[SENDER_SZ];
    char snippet[SNIP_SZ];
} sms_thread_item_t;

static sms_thread_item_t s_threads[MAX_THREADS];

static void threads_clear(void) { memset(s_threads, 0, sizeof(s_threads)); }

typedef enum {
    THREADS_GROUP_SENDER = 0,
    THREADS_GROUP_THREAD_ID = 1,
} threads_mode_t;

static threads_mode_t s_mode = THREADS_GROUP_SENDER;

static lv_obj_t *s_list = NULL;
static lv_obj_t *s_btn_mode = NULL;
static lv_obj_t *s_lbl_mode = NULL;

static const char *mode_label(threads_mode_t m)
{
    return (m == THREADS_GROUP_SENDER) ? "Group: Sender" : "Group: Thread";
}
static const char *app_symbol_for_key(const char *k)
{
    if (!k) return LV_SYMBOL_FILE;

    // Match common package names / ids
    if (strstr(k, "com.facebook.orca"))    return LV_SYMBOL_BELL;   // Messenger (placeholder)
    if (strstr(k, "discord"))              return LV_SYMBOL_WIFI;   // Discord (placeholder)
    if (strstr(k, "com.google.android.youtube")) return LV_SYMBOL_PLAY;
    if (strstr(k, "com.reddit"))           return LV_SYMBOL_DIRECTORY;
    if (strstr(k, "com.textra"))           return LV_SYMBOL_EDIT;   // SMS app
    if (strstr(k, "amazon"))               return LV_SYMBOL_HOME;

    return LV_SYMBOL_FILE;
}

static const char *app_friendly_name(const char *k)
{
    if (!k) return "App";
    if (strstr(k, "com.facebook.orca")) return "Messenger";
    if (strstr(k, "discord"))           return "Discord";
    if (strstr(k, "com.google.android.youtube")) return "YouTube";
    if (strstr(k, "com.reddit"))        return "Reddit";
    if (strstr(k, "com.textra"))        return "SMS";
    if (strstr(k, "amazon"))            return "Amazon";
    return "App";
}

static const char *thread_key_for_item(const sms_thread_item_t *it, threads_mode_t m)
{
    return (m == THREADS_GROUP_SENDER) ? it->sender : it->thread_id;
}

static int threads_find_by_key(threads_mode_t m, const char *key)
{
    if (!key) return -1;

    for (int i = 0; i < MAX_THREADS; i++) {
        if (!s_threads[i].used) continue;

        const char *k = (m == THREADS_GROUP_SENDER) ? s_threads[i].sender : s_threads[i].thread_id;
        if (strcmp(k, key) == 0) return i;
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
    char *save = NULL;

    (void)strtok_r(line, "|", &save);            // msg_id
    char *ts   = strtok_r(NULL, "|", &save);     // ts_ms
    char *tid  = strtok_r(NULL, "|", &save);     // thread_id
    char *snd  = strtok_r(NULL, "|", &save);     // sender
    (void)strtok_r(NULL, "|", &save);            // pkg
    char *body = strtok_r(NULL, "|", &save);     // body

    if (!ts || !tid || !snd || !body) return;

    uint64_t tms = (uint64_t)strtoull(ts, NULL, 10);

    const char *key = (s_mode == THREADS_GROUP_SENDER) ? snd : tid;

    int idx = threads_find_by_key(s_mode, key);
    if (idx < 0) idx = threads_free_slot();
    if (idx < 0) return;

    // Keep the most recent record per group
    if (s_threads[idx].used && tms <= s_threads[idx].ts_ms) return;

    s_threads[idx].used  = true;
    s_threads[idx].ts_ms = tms;

    // Store BOTH values so either mode can display nicely
    strlcpy0(s_threads[idx].thread_id, sizeof(s_threads[idx].thread_id), tid);
    strlcpy0(s_threads[idx].sender,    sizeof(s_threads[idx].sender),    snd);
    snip_copy(s_threads[idx].snippet,  sizeof(s_threads[idx].snippet),   body);
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

// static esp_err_t load_log_work(void *p)
// {
//     load_ctx_t *c = (load_ctx_t*)p;
//     return watch_sdcard_read_file(SMS_LOG_PATH, &c->buf, &c->len, MAX_LOG_BYTES, 0);
// }

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

    ui_sms_set_active_thread(it->thread_id, it->sender); // name can be sender for now

    // Keep sender too if you want, but thread_id becomes primary
    ui_sms_set_active_sender(it->sender);

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
        const char *k = thread_key_for_item(&s_threads[i], s_mode);
        snprintf(title, sizeof(title), "%s", k);


        lv_obj_t *btn = lv_list_add_btn(s_list, LV_SYMBOL_CALL, title);
                /* FIX: style sender (title) label */
        lv_obj_t *sender_lbl = lv_obj_get_child(btn, 1); // 0=icon, 1=text
        if (sender_lbl) {
            lv_obj_set_style_text_color(sender_lbl, UI_COLOR(THEME), 0);
            lv_obj_set_style_text_font(sender_lbl, &lv_font_montserrat_16, 0);
        }
        lv_obj_add_event_cb(btn, on_thread_click, LV_EVENT_CLICKED, &s_threads[i]);

        /* Card styling */
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x101010), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_90, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x303030), 0);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_pad_all(btn, 10, 0);
        lv_obj_set_style_pad_row(btn, 6, 0);

        // small snippet label under the button (LVGL list item is a button; add a label child)
        lv_obj_t *sn = lv_label_create(btn);
        lv_label_set_text(sn, s_threads[i].snippet);
        lv_obj_set_style_text_color(sn, UI_COLOR(THEME), 0);   // dim gray, or UI_COLOR(THEME) with low opa
        lv_obj_set_style_text_opa(sn, LV_OPA_90, 0);
        lv_obj_set_style_text_font(sn, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(sn, LV_LABEL_LONG_DOT);
        lv_obj_set_width(sn, lv_pct(65));
        lv_obj_align(sn, LV_ALIGN_BOTTOM_LEFT, 42, -2);

    }

    if (!any) {
        lv_obj_t *lbl = lv_label_create(s_list);
        lv_label_set_text(lbl, "No SMS yet.\n(Need messages.log)");
        lv_obj_center(lbl);
    }
}

static void on_mode_toggle(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    s_mode = (s_mode == THREADS_GROUP_SENDER) ? THREADS_GROUP_THREAD_ID : THREADS_GROUP_SENDER;

    if (s_lbl_mode) lv_label_set_text(s_lbl_mode, mode_label(s_mode));

    threads_load_from_sd();
    threads_render();
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

    lv_obj_set_style_bg_opa(s_list, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 10, 0);
    
    // Mode toggle button (top-right)
    s_btn_mode = lv_btn_create(scr);
    lv_obj_set_size(s_btn_mode, 150, 40);
    lv_obj_align(s_btn_mode, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_add_event_cb(s_btn_mode, on_mode_toggle, LV_EVENT_CLICKED, NULL);

    s_lbl_mode = lv_label_create(s_btn_mode);
    lv_label_set_text(s_lbl_mode, mode_label(s_mode));
    lv_obj_center(s_lbl_mode);

    // Optional styling to match theme
    lv_obj_set_style_bg_color(s_btn_mode, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(s_btn_mode, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_btn_mode, 1, 0);
    lv_obj_set_style_border_color(s_btn_mode, lv_color_hex(0x303030), 0);
    lv_obj_set_style_radius(s_btn_mode, 10, 0);
    lv_obj_set_style_text_color(s_lbl_mode, UI_COLOR(THEME), 0);
    lv_obj_set_style_text_font(s_lbl_mode, &lv_font_montserrat_14, 0);
    threads_load_from_sd();
    threads_render();
    return scr;
}
