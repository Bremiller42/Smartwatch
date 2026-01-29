#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    STO_SRC_SETTINGS = 0,   // persistent default (from settings/NVS)
    STO_SRC_OVERRIDE = 1,   // temporary (per-screen / modal)
} sto_src_t;

// Init with current default (loaded from NVS or compile-time)
void screen_timeout_init(uint32_t default_ms);

// Persistent default (what Settings changes)
void     screen_timeout_set_default_ms(uint32_t ms);
uint32_t screen_timeout_get_default_ms(void);

// Persistent Always-On (Settings toggle)
// When enabled, effective timeout becomes 0 (never).
void screen_timeout_set_always_on(bool on);
bool screen_timeout_get_always_on(void);

// Effective timeout (default or top override), with Always-On applied
uint32_t screen_timeout_get_effective_ms(void);
bool     screen_timeout_is_overridden(void);

// Override stack (supports nesting: log screen -> modal -> etc)
int  screen_timeout_push_override_ms(uint32_t ms); // returns token/handle (>=0) or -1
void screen_timeout_pop_override(int token);       // safe: ignores wrong token
void screen_timeout_clear_overrides(void);

// Convenience
static inline int  screen_always_on(void) { return screen_timeout_push_override_ms(0); } // 0 = never time out
static inline void screen_timeout_restore(int token) { screen_timeout_pop_override(token); }

// Activity hook (call this instead of touching raw globals)
void screen_timeout_mark_activity(void);

// Tick/Query for your sleep/power manager
bool     screen_timeout_expired(uint32_t now_ms);   // now_ms = esp_timer_get_time()/1000, etc
uint32_t screen_timeout_deadline_ms(void);          // for debugging/UI

/* NEW: simple global override API */
void     screen_keep_awake_set(bool on);
bool     screen_keep_awake_get(void);
bool     screen_keep_awake_toggle(void);

/* NEW: safe multi-caller hold/release */
void     screen_keep_awake_acquire(void);
void     screen_keep_awake_release(void);
uint32_t screen_keep_awake_refcount(void);