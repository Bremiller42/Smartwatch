#include "watch_screen_timeout.h"
#include "esp_log.h"
#include "esp_timer.h"

#define STO_MAX_STACK 8
static const char *TAG = "SCR_TO";

typedef struct {
    uint32_t ms;
    int      token;
} sto_entry_t;

static sto_entry_t s_stack[STO_MAX_STACK];
static int s_stack_len = 0;
static int s_next_token = 1;

static uint32_t s_default_ms = 15000;
static uint32_t s_deadline_ms = 0;
static bool     s_deadline_valid = false;

static uint32_t effective_ms(void)
{
    if (s_stack_len > 0) return s_stack[s_stack_len - 1].ms;
    return s_default_ms;
}

static void recompute_deadline(uint32_t now_ms)
{
    uint32_t ms = effective_ms();
    if (ms == 0) {
        s_deadline_valid = false;      // "always on"
        s_deadline_ms = 0;
        return;
    }
    s_deadline_valid = true;
    s_deadline_ms = now_ms + ms;
}

void screen_timeout_init(uint32_t default_ms)
{
    if (default_ms < 1000) default_ms = 1000;
    s_default_ms = default_ms;
    s_stack_len = 0;
    s_deadline_valid = false;
    s_deadline_ms = 0;
}

void screen_timeout_set_default_ms(uint32_t ms)
{
    if (ms < 1000) ms = 1000;
    s_default_ms = ms;

    // if no override, default change affects effective -> refresh deadline
    // (call mark_activity from UI when changing settings; this is just safety)
}

uint32_t screen_timeout_get_default_ms(void) { return s_default_ms; }
uint32_t screen_timeout_get_effective_ms(void) { return effective_ms(); }
bool screen_timeout_is_overridden(void) { return s_stack_len > 0; }

int screen_timeout_push_override_ms(uint32_t ms)
{
    if (s_stack_len >= STO_MAX_STACK) {
        ESP_LOGW(TAG, "override stack full");
        return -1;
    }
    int token = s_next_token++;
    s_stack[s_stack_len++] = (sto_entry_t){ .ms = ms, .token = token };
    return token;
}

void screen_timeout_pop_override(int token)
{
    if (s_stack_len <= 0) return;

    // only allow popping the top (simple + safe). If mismatch, ignore.
    if (s_stack[s_stack_len - 1].token != token) {
        ESP_LOGW(TAG, "pop token mismatch (top=%d, got=%d)", s_stack[s_stack_len - 1].token, token);
        return;
    }
    s_stack_len--;
}

void screen_timeout_clear_overrides(void)
{
    s_stack_len = 0;
}

void screen_timeout_mark_activity(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    recompute_deadline(now_ms);
}

bool screen_timeout_expired(uint32_t now_ms)
{
    if (!s_deadline_valid) return false; // always-on
    return (now_ms >= s_deadline_ms);
}

uint32_t screen_timeout_deadline_ms(void)
{
    return s_deadline_valid ? s_deadline_ms : 0;
}
