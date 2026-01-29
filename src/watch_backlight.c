// FILE: src/watch_backlight.c
#include "watch_backlight.h"

#include <stdio.h>
#include <stdbool.h>
#include "watch_globals.h" // g_brightness
#include "esp_log.h"
#include "display.h" // bsp_display_brightness_set()

static const char *TAG = "BKL";

// State
static int  s_user_pct  = 80;   // 0..100
static int  s_cap_pct   = 100;  // 0..100
static bool s_screen_on = true; // if false -> force PWM=0

static int clamp_pct(int v)
{
    if (v < 0)   return 0;
    if (v > 100) return 100;
    return v;
}

static int effective_pct(void)
{
    const int u = s_user_pct;
    const int c = s_cap_pct;
    return (u < c) ? u : c;
}

void backlight_apply_now(void)
{
    const int eff = effective_pct();

    if (!s_screen_on) {
        bsp_display_brightness_set(0);
        ESP_LOGD(TAG, "apply: screen_off -> pwm=0 (cap=%d user=%d eff=%d)",
                 s_cap_pct, s_user_pct, eff);
        return;
    }

    bsp_display_brightness_set(eff);
    ESP_LOGW(TAG, "cap=%d user=%d eff=%d", s_cap_pct, s_user_pct, eff);
}

void backlight_init(int persisted_user_pct)
{
    s_user_pct  = clamp_pct(persisted_user_pct);
    if (s_user_pct < 10) {
        s_user_pct = 10; // minimum user brightness
        g_brightness = s_user_pct; // update global
    }
    s_cap_pct   = 100;
    s_screen_on = true;

    ESP_LOGI(TAG, "init user=%d cap=%d eff=%d",
             s_user_pct, s_cap_pct, effective_pct());

    backlight_apply_now();
}

void backlight_set_screen_on(bool on)
{
    if (s_screen_on == on) return;
    s_screen_on = on;
    backlight_apply_now();
}

void backlight_set_user_pct(int pct)
{
    s_user_pct = clamp_pct(pct);
    ESP_LOGI(TAG, "user set -> %d (cap=%d eff=%d)", s_user_pct, s_cap_pct, effective_pct());

    // IMPORTANT: do not touch hardware directly; respect screen gating
    backlight_apply_now();
}

int backlight_get_user_pct(void)
{
    return s_user_pct;
}

void backlight_set_cap_pct(int pct)
{
    s_cap_pct = clamp_pct(pct);
    ESP_LOGI(TAG, "cap set -> %d (user=%d eff=%d)", s_cap_pct, s_user_pct, effective_pct());

    // IMPORTANT: do not touch hardware directly; respect screen gating
    backlight_apply_now();
}

int backlight_get_cap_pct(void)
{
    return s_cap_pct;
}

int backlight_get_effective_pct(void)
{
    return effective_pct();
}

bool backlight_is_capped(void)
{
    return s_cap_pct < s_user_pct;
}

bool backlight_is_screen_on(void)
{
    return s_screen_on;
}
