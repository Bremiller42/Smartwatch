#include "ui_color_pallete.h"

const uint32_t color_table[COLOR_COUNT] = {
    [BLACK]       = 0x000000,  // PURE BLACK
    [WHITE]       = 0xE6E8EB,  // Soft white (less glare)

    [RED]         = 0xAD0E0E,  // Muted warning red
    [DARK_RED]    = 0x4A1C1C,  // Deep maroon (errors / critical)

    [ORANGE]      = 0xCC680E,  // Warm amber (alerts, battery low)
    [YELLOW]      = 0xCFC10A,  // Soft gold (not highlighter-yellow)

    [GREEN]       = 0x3FAE8F,  // Teal-green (eye-safe, modern)
    [BLUE]        = 0x2962FF,  // Bluetooth blue 🔵

    [CYAN]        = 0x00FFFF,  // (unchanged) THEME anchor
    [THEME]       = 0x00E5FF,  // Your device cyan

    [MAGENTA]     = 0xC062D6,  // Soft violet-magenta (not Barbie pink)
};

ColorID battery_color(int percent)
{
    if (percent >= 75) return GREEN;
    if (percent >= 40) return YELLOW;
    if (percent >= 20) return ORANGE;
    return RED;
}
