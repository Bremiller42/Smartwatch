// FILE: src/ui/ui_screen_home.c
#include "ui_priv.h"
#include "esp_log.h"
#include "ui_color_pallete.h"

static const char *UI_SH_TAG = "UI_HOME";

static void on_open_clock(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_show(UI_CLOCK);
}

static void on_open_settings(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_show(UI_SETTINGS);
}

static void on_log_open(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_show(UI_LOG);
}

static void on_pwr_open(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ui_show(UI_POWER_MENU);
}

lv_obj_t *ui_build_home_screen(void)
{
    ESP_LOGI(UI_SH_TAG, "Building Home Screen");

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // keep screen-timeout / activity plumbing consistent
    lv_obj_add_event_cb(scr, activity_event_cb, LV_EVENT_ALL, NULL);

    // ---- Grid container ----
    // 2 cols x 2 rows, centered, with padding/gaps like Settings
    static lv_coord_t col_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static lv_coord_t row_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };

    // Safe UI margins
    #define PAD_SIDE   14
    #define PAD_BOTTOM 14
    #define PAD_TOP    (PAD_SIDE + (PAD_SIDE / 4))   // +25% top buffer

    lv_obj_t *safe = lv_obj_create(scr);
    lv_obj_set_size(safe, lv_pct(100), lv_pct(100));
    lv_obj_center(safe);
    lv_obj_set_style_bg_opa(safe, LV_OPA_0, 0);
    lv_obj_set_style_border_width(safe, 0, 0);
    lv_obj_clear_flag(safe, LV_OBJ_FLAG_SCROLLABLE);

    // Outer padding (this constrains everything inside)
    lv_obj_set_style_pad_left(safe, PAD_SIDE, 0);
    lv_obj_set_style_pad_right(safe, PAD_SIDE, 0);
    lv_obj_set_style_pad_bottom(safe, PAD_BOTTOM, 0);
    lv_obj_set_style_pad_top(safe, PAD_TOP, 0);

    lv_obj_t *g = lv_obj_create(safe);
    lv_obj_set_size(g, lv_pct(100), lv_pct(100));
    lv_obj_center(g);
    lv_obj_set_style_bg_opa(g, LV_OPA_0, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);

    // Tile spacing inside grid
    lv_obj_set_style_pad_row(g, 14, 0);
    lv_obj_set_style_pad_column(g, 14, 0);

    lv_obj_set_grid_dsc_array(g, col_dsc, row_dsc);


    // ---- Tiles (reuse the same helpers you used in Settings) ----
    // Top-left: Clock
    lv_obj_t *t_clock = tile_create_nav_tile(g, "Clock", "Open", on_open_clock);
    tile_set_on(t_clock, true);
    lv_obj_set_grid_cell(t_clock,
        LV_GRID_ALIGN_STRETCH, 0, 1,
        LV_GRID_ALIGN_STRETCH, 0, 1);

    // Bottom-left: Settings
    lv_obj_t *t_settings = tile_create_nav_tile(g, "Settings", "Configure", on_open_settings);
    tile_set_on(t_settings, true);
    lv_obj_set_grid_cell(t_settings,
        LV_GRID_ALIGN_STRETCH, 0, 1,
        LV_GRID_ALIGN_STRETCH, 1, 1);

    // Top-right: Log
    lv_obj_t *t_log = tile_create_nav_tile(g, "Log", "View", on_log_open);
    tile_set_on(t_log, true);
    lv_obj_set_grid_cell(t_log,
        LV_GRID_ALIGN_STRETCH, 1, 1,
        LV_GRID_ALIGN_STRETCH, 0, 1);

    // Bottom-right: (placeholder / future app)
    lv_obj_t *t_more = tile_create_base(g, "More", "Later", NULL);
    tile_set_on(t_more, false);
    lv_obj_set_grid_cell(t_more,
        LV_GRID_ALIGN_STRETCH, 1, 1,
        LV_GRID_ALIGN_STRETCH, 1, 1);

    // ---- Power button overlay (top-right) ----
    lv_obj_t *btn_pwr = lv_btn_create(scr);
    lv_obj_set_size(btn_pwr, 50, 50);
    lv_obj_align(btn_pwr, LV_ALIGN_TOP_RIGHT, -2, 2);
    lv_obj_add_event_cb(btn_pwr, on_pwr_open, LV_EVENT_CLICKED, NULL);

    // Transparent button background
    lv_obj_set_style_bg_opa(btn_pwr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_pwr, 0, 0);

    lv_obj_t *pwr_icon = lv_label_create(btn_pwr);
    lv_label_set_text(pwr_icon, LV_SYMBOL_POWER);
    lv_obj_center(pwr_icon);
    lv_obj_set_style_text_font(pwr_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(pwr_icon, UI_COLOR(RED), 0);

    return scr;
}
