// FILE: src/ui/ui_notif_bar.c
#include "ui_priv.h"
#include "esp_log.h"
#include "../watch_icons/watch_icons.h"
#include "watch_audio.h"
static const char *UI_NOTI_TAG = "UI_NOTIF";


#define NOTIF_ICON_W 40
#define NOTIF_ICON_H 40
#define NOTIF_GAP    10
#define NOTIF_COLS   7
#define NOTIF_BAR_TOP_Y 120
#define NOTIF_BAR_W_PCT 92

static lv_obj_t *notif_bar = NULL;
static lv_obj_t *notif_slot[NG_MAX] = {0};
static lv_obj_t *notif_icon_img[NG_MAX] = {0};
static lv_obj_t *notif_badge_lbl[NG_MAX] = {0};
static lv_obj_t *notif_badge_stroke[NG_MAX][4] = {0};

static lv_font_t *font_badge_bold = (lv_font_t *)&lv_font_montserrat_22;

static const lv_img_dsc_t *icon_for_group(notif_type_t g)
{
    switch (g) {
        case NG_YOUTUBE:   return &icon_youtube_brands_solid;
        case NG_REDDIT:    return &icon_reddit_brands_solid;
        case NG_DISCORD:   return &icon_discord_brands_solid;
        case NG_AMAZON:    return &icon_amazon_brands_solid;

        case NG_MESSENGER: return &icon_comments_regular;
        case NG_META:      return &icon_meta_brands_solid;

        case NG_WEATHER:   return &icon_cloud_sun_solid;
        case NG_EMAIL:     return &icon_envelope_regular;
        case NG_SMS:       return &icon_message_regular;
        case NG_SYSTEM:    return &icon_desktop_solid;

        case NG_APP:       return &icon_gear_solid;
        default:           return NULL;
    }
}

static void notif_bar_relayout(void)
{
    if (!notif_bar) return;

    int visible_count = 0;
    for (int i = 0; i < NG_MAX; i++) {
        if (!notif_slot[i]) continue;
        if (lv_obj_has_flag(notif_slot[i], LV_OBJ_FLAG_HIDDEN)) continue;
        visible_count++;
    }

    if (visible_count == 0) {
        lv_obj_set_height(notif_bar, 0);
        lv_obj_align(notif_bar, LV_ALIGN_TOP_MID, 0, NOTIF_BAR_TOP_Y);
        return;
    }

    int rows = (visible_count + NOTIF_COLS - 1) / NOTIF_COLS;
    lv_coord_t h = (rows * NOTIF_ICON_H) + ((rows - 1) * NOTIF_GAP);

    lv_obj_set_height(notif_bar, h);
    lv_obj_align(notif_bar, LV_ALIGN_TOP_MID, 0, NOTIF_BAR_TOP_Y);

    int visible_idx = 0;
    for (int i = 0; i < NG_MAX; i++) {
        if (!notif_slot[i]) continue;
        if (lv_obj_has_flag(notif_slot[i], LV_OBJ_FLAG_HIDDEN)) continue;

        int row = visible_idx / NOTIF_COLS;
        int col = visible_idx % NOTIF_COLS;

        int x = col * (NOTIF_ICON_W + NOTIF_GAP);
        int y = row * (NOTIF_ICON_H + NOTIF_GAP);

        lv_obj_set_pos(notif_slot[i], x, y);
        visible_idx++;
    }
}

static void notif_icons_refresh(void)
{
    if (!notif_bar) return;

    int visible_count = 0;

    for (int i = 0; i < NG_MAX; i++) {
        if (!notif_slot[i]) continue;

        uint16_t c = g_notif_counts[i];

        if (c == 0) {
            lv_obj_add_flag(notif_slot[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        visible_count++;
        lv_obj_clear_flag(notif_slot[i], LV_OBJ_FLAG_HIDDEN);

        char b[8];
        if (c > 99) strcpy(b, "99+");
        else snprintf(b, sizeof(b), "%u", (unsigned)c);

        if (notif_badge_lbl[i]) lv_label_set_text(notif_badge_lbl[i], b);
        for (int k = 0; k < 4; k++) {
            if (notif_badge_stroke[i][k]) lv_label_set_text(notif_badge_stroke[i][k], b);
        }
    }

    if (visible_count == 0) {
        lv_obj_add_flag(notif_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(notif_bar, LV_OBJ_FLAG_HIDDEN);
    notif_bar_relayout();
}

static void ui_notif_refresh_cb(void *arg)
{
    (void)arg;
    notif_icons_refresh();
    g_notif_dirty = false;
}

void ui_notif_refresh_async(void)
{
    lv_async_call(ui_notif_refresh_cb, NULL);
}

void ui_notif_add(notif_type_t t)
{
    if (t < 0 || t >= NG_MAX) return;
    if (g_notif_counts[t] < 999) g_notif_counts[t]++;
    watch_audio_beep(2000, 70);
    watch_audio_beep(1500, 70);

    g_notif_dirty = true;
    ui_notif_refresh_async();

    ESP_LOGI(UI_NOTI_TAG, "notif add type=%d count=%u", (int)t, (unsigned)g_notif_counts[t]);
}

void ui_notif_clear_type(notif_type_t t)
{
    if (t < 0 || t >= NG_MAX) return;
    g_notif_counts[t] = 0;

    g_notif_dirty = true;
    ui_notif_refresh_async();
}

void ui_notif_clear_all(void)
{
    for (int i = 0; i < NG_MAX; i++) g_notif_counts[i] = 0;

    g_notif_dirty = true;
    ui_notif_refresh_async();
}

static void ui_notif_add_async_cb(void *arg)
{
    notif_type_t t = (notif_type_t)(intptr_t)arg;
    ui_notif_add(t);
}

void ui_notif_add_from_ble(notif_type_t t)
{
    lv_async_call(ui_notif_add_async_cb, (void*)(intptr_t)t);
}

/* Called once when clock screen is built */
void ui_notif_bar_attach(lv_obj_t *clock_screen_parent)
{
    // If clock screen rebuilds in future, clean old bar
    notif_bar = NULL;
    for (int i = 0; i < NG_MAX; i++) {
        notif_slot[i] = NULL;
        notif_icon_img[i] = NULL;
        notif_badge_lbl[i] = NULL;
        for (int k = 0; k < 4; k++) notif_badge_stroke[i][k] = NULL;
    }

    notif_bar = lv_obj_create(clock_screen_parent);
    lv_obj_set_width(notif_bar, lv_pct(NOTIF_BAR_W_PCT));
    lv_obj_set_height(notif_bar, 0);
    lv_obj_set_style_bg_opa(notif_bar, LV_OPA_0, 0);
    lv_obj_set_style_border_width(notif_bar, 0, 0);
    lv_obj_clear_flag(notif_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(notif_bar, LV_ALIGN_TOP_MID, 0, NOTIF_BAR_TOP_Y);
    lv_obj_set_style_clip_corner(notif_bar, false, 0);
    lv_obj_set_style_radius(notif_bar, 0, 0);
    lv_obj_set_style_pad_all(notif_bar, 0, 0);

    for (int i = 0; i < NG_MAX; i++) {
        notif_slot[i] = lv_obj_create(notif_bar);
        lv_obj_set_size(notif_slot[i], NOTIF_ICON_W, NOTIF_ICON_H);
        lv_obj_set_style_radius(notif_slot[i], 8, 0);
        lv_obj_set_style_bg_color(notif_slot[i], lv_color_hex(0x202020), 0);
        lv_obj_set_style_bg_opa(notif_slot[i], LV_OPA_70, 0);
        lv_obj_set_style_border_width(notif_slot[i], 1, 0);
        lv_obj_set_style_border_color(notif_slot[i], lv_color_hex(0x505050), 0);
        lv_obj_clear_flag(notif_slot[i], LV_OBJ_FLAG_SCROLLABLE);

        notif_icon_img[i] = lv_img_create(notif_slot[i]);
        const lv_img_dsc_t *src = icon_for_group((notif_type_t)i);
        if (src) {
            lv_img_set_src(notif_icon_img[i], src);
            lv_obj_set_style_img_recolor(notif_icon_img[i], lv_color_white(), 0);
            lv_obj_set_style_img_recolor_opa(notif_icon_img[i], LV_OPA_COVER, 0);
            lv_obj_center(notif_icon_img[i]);
        } else {
            lv_obj_add_flag(notif_icon_img[i], LV_OBJ_FLAG_HIDDEN);
        }

        static const lv_coord_t dx[4] = { -1,  1,  0,  0 };
        static const lv_coord_t dy[4] = {  0,  0, -1,  1 };

        for (int k = 0; k < 4; k++) {
            notif_badge_stroke[i][k] = lv_label_create(notif_slot[i]);
            lv_obj_set_style_text_color(notif_badge_stroke[i][k], lv_color_black(), 0);
            lv_obj_set_style_text_font(notif_badge_stroke[i][k], font_badge_bold, 0);
            lv_label_set_text(notif_badge_stroke[i][k], "");
            lv_obj_align(notif_badge_stroke[i][k], LV_ALIGN_BOTTOM_RIGHT, 13 + dx[k], 14 + dy[k]);
        }

        notif_badge_lbl[i] = lv_label_create(notif_slot[i]);
        lv_obj_set_style_text_color(notif_badge_lbl[i], lv_color_white(), 0);
        lv_obj_set_style_text_font(notif_badge_lbl[i], font_badge_bold, 0);
        lv_label_set_text(notif_badge_lbl[i], "");
        lv_obj_align(notif_badge_lbl[i], LV_ALIGN_BOTTOM_RIGHT, 13, 14);
        lv_obj_move_foreground(notif_badge_lbl[i]);

        lv_obj_add_flag(notif_slot[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_flag(notif_bar, LV_OBJ_FLAG_HIDDEN);

    // reflect any existing counts
    notif_icons_refresh();
}
