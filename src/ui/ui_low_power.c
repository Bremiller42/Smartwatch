// FILE: src/ui/ui_low_power.c
#include "ui_priv.h"
#include "esp_log.h"
#include "ui_color_pallete.h"
static const char *UI_SB_TAG = "UI_SCN_LPWR";

lv_obj_t *ui_build_low_pwr_screen(void)
{
    ESP_LOGI(UI_SB_TAG, "Low Power Screen");

    lv_obj_t *scr_low_pwr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_low_pwr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr_low_pwr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_low_pwr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lp_label = lv_label_create(scr_low_pwr);
    lv_label_set_text(lp_label, "Low Power");
    lv_obj_set_style_text_font(lp_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lp_label, UI_COLOR(THEME), 0);

    lv_obj_set_style_bg_color(lp_label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lp_label, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lp_label, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(lp_label, 12, LV_PART_MAIN);
    lv_obj_clear_flag(lp_label, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_align(lp_label, LV_ALIGN_CENTER, 0, 0);

    return scr_low_pwr;

}
