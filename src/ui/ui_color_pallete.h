#pragma once
#include <stdint.h>
#include "lvgl.h"

typedef enum {
    BLACK,
    WHITE,
    RED,
    DARK_RED,
    ORANGE,
    GREEN,
    BLUE,
    YELLOW,
    CYAN,
    THEME,
    MAGENTA,

    COLOR_COUNT
} ColorID;

// declare the table (no storage here!)
extern const uint32_t color_table[COLOR_COUNT];

#define UI_COLOR(id) lv_color_hex(color_table[(id)])

// declare the function (no body here!)
ColorID battery_color(int percent);
