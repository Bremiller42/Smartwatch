// FILE: src/watch_backlight.h
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize backlight manager with current persisted user brightness (0..100).
// Safe to call once during boot after settings are loaded.
void backlight_init(int persisted_user_pct);

// User preference (saved to NVS by settings layer)
// Sets user brightness and immediately reapplies effective brightness.
void backlight_set_user_pct(int pct);
int  backlight_get_user_pct(void);

// Policy cap (battery/thermal/etc). Default 100.
// Sets cap and immediately reapplies effective brightness.
// DOES NOT change user preference.
void backlight_set_cap_pct(int pct);
int  backlight_get_cap_pct(void);

// Effective brightness actually applied to hardware.
int  backlight_get_effective_pct(void);

// Force re-apply effective brightness to hardware (rarely needed)
void backlight_apply_now(void);

// Optional convenience: “is capped by policy?”
bool backlight_is_capped(void);
void backlight_set_screen_on(bool on);

#ifdef __cplusplus
}
#endif
