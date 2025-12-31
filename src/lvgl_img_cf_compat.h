#pragma once

#include "lvgl.h"

/* LVGL v8/v9 naming differences for image color formats */
#ifndef LV_IMG_CF_ALPHA_8
  #ifdef LV_IMG_CF_ALPHA_8BIT
    #define LV_IMG_CF_ALPHA_8 LV_IMG_CF_ALPHA_8BIT
  #endif
#endif
