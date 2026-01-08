// FILE: src/ui/ui_screen_blank.c
#include "ui_priv.h"
#include "esp_log.h"
static const char *UI_SB_TAG = "UI_SCN_BLK";

lv_obj_t *ui_build_black_screen(void)
{
    ESP_LOGI(UI_SB_TAG, "Blanking Screen");

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}
