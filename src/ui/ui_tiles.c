// FILE: src/ui/ui_tiles.c
#include "ui_priv.h"
#include "esp_log.h"
static const char *UI_TIL_TAG = "UI_TILES";

#define TILE_W   130
#define TILE_H   100
#define TILE_RAD 13

static lv_color_t TILE_OFF_BG(void) { return lv_color_hex(0x2A2A2A); }
static lv_color_t TILE_ON_BG(void)  { return lv_color_hex(0x1E6BFF); }
static lv_color_t TILE_BORDER(void) { return lv_color_hex(0x3A3A3A); }

typedef struct {
    lv_obj_t *tile_btn;
    lv_obj_t *hidden_sw;
    lv_obj_t *sub_lbl;
} tile_switch_ctx_t;

static void tile_style_base(lv_obj_t *btn)
{
    lv_obj_set_style_radius(btn, TILE_RAD, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, TILE_BORDER(), 0);
    lv_obj_set_style_pad_all(btn, 12, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
}

void tile_set_on(lv_obj_t *btn, bool on)
{
    lv_obj_set_style_bg_color(btn, on ? TILE_ON_BG() : TILE_OFF_BG(), 0);
}

lv_obj_t *tile_create_base(lv_obj_t *parent, const char *title, const char *subtitle, lv_obj_t **out_sub_lbl)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, TILE_W, TILE_H);
    tile_style_base(btn);
    tile_set_on(btn, false);

    lv_obj_t *t = lv_label_create(btn);
    lv_label_set_text(t, title ? title : "");
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *s = lv_label_create(btn);
    lv_label_set_text(s, subtitle ? subtitle : "");
    lv_obj_set_style_text_color(s, lv_color_white(), 0);
    lv_obj_set_style_text_opa(s, LV_OPA_80, 0);
    lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    if (out_sub_lbl) *out_sub_lbl = s;
    return btn;
}

lv_obj_t *tile_create_nav_tile(lv_obj_t *parent, const char *title, const char *subtitle, lv_event_cb_t on_click)
{
    lv_obj_t *sub = NULL;
    lv_obj_t *btn = tile_create_base(parent, title, subtitle, &sub);

    tile_set_on(btn, true);

    if (on_click) lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, lv_color_white(), 0);
    lv_obj_set_style_text_opa(arrow, LV_OPA_70, 0);
    lv_obj_align(arrow, LV_ALIGN_TOP_RIGHT, 0, 0);

    return btn;
}

static void on_switch_tile_clicked(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    tile_switch_ctx_t *ctx = (tile_switch_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !ctx->tile_btn || !ctx->hidden_sw) return;

    bool now_on = !lv_obj_has_state(ctx->hidden_sw, LV_STATE_CHECKED);

    if (now_on) lv_obj_add_state(ctx->hidden_sw, LV_STATE_CHECKED);
    else        lv_obj_clear_state(ctx->hidden_sw, LV_STATE_CHECKED);

    tile_set_on(ctx->tile_btn, now_on);

    lv_event_send(ctx->hidden_sw, LV_EVENT_VALUE_CHANGED, NULL);

    mark_user_activity();
    ESP_LOGI(UI_TIL_TAG, "Tile Switched");

}

static void on_tile_ctx_cleanup(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    tile_switch_ctx_t *ctx = (tile_switch_ctx_t *)lv_event_get_user_data(e);
    if (ctx) lv_mem_free(ctx);
}

lv_obj_t *tile_create_switch_tile(lv_obj_t *parent,
                                 const char *title,
                                 const char *subtitle,
                                 bool initial_on,
                                 lv_event_cb_t on_switch_value_changed,
                                 lv_obj_t **out_sub_lbl)
{
    lv_obj_t *sub = NULL;
    lv_obj_t *btn = tile_create_base(parent, title, subtitle, &sub);

    lv_obj_t *sw = lv_switch_create(btn);
    lv_obj_add_flag(sw, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(sw, 1, 1);
    lv_obj_set_style_opa(sw, LV_OPA_0, 0);

    if (initial_on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else            lv_obj_clear_state(sw, LV_STATE_CHECKED);

    if (on_switch_value_changed) {
        lv_obj_add_event_cb(sw, on_switch_value_changed, LV_EVENT_VALUE_CHANGED, NULL);
    }

    tile_set_on(btn, initial_on);

    tile_switch_ctx_t *ctx = (tile_switch_ctx_t *)lv_mem_alloc(sizeof(tile_switch_ctx_t));
    ctx->tile_btn  = btn;
    ctx->hidden_sw = sw;
    ctx->sub_lbl   = sub;

    lv_obj_add_event_cb(btn, on_switch_tile_clicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(btn, on_tile_ctx_cleanup,   LV_EVENT_DELETE,  ctx);

    if (out_sub_lbl) *out_sub_lbl = sub;
    return btn;
}
