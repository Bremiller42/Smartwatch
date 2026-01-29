// FILE: src/watch_screen_timeout.c
#include "watch_screen_timeout.h"

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "SCR_TO";

/* Default timeout (ms) from Settings */
static uint32_t s_default_ms = 15000;

/* Persistent Always-On from Settings */
static bool s_always_on = false;

/* Simple temporary keep-awake override (refcounted) */
static uint32_t s_keep_awake_refs = 0;

/* Deadline bookkeeping */
static uint32_t s_deadline_ms = 0;
static bool     s_deadline_valid = false;

static inline uint32_t now_ms_local(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool keep_awake_active(void)
{
    return (s_keep_awake_refs > 0);
}

uint32_t screen_timeout_get_default_ms(void)
{
    return s_default_ms;
}

void screen_timeout_set_default_ms(uint32_t ms)
{
    if (ms < 1000) ms = 1000;
    s_default_ms = ms;
    screen_timeout_mark_activity();
}

bool screen_timeout_get_always_on(void)
{
    return s_always_on;
}

void screen_timeout_set_always_on(bool on)
{
    if (s_always_on == on) return;
    s_always_on = on;
    screen_timeout_mark_activity();
    ESP_LOGI(TAG, "Always-On (persistent) -> %d", (int)on);
}

/* Effective timeout:
 * - persistent always-on => never timeout
 * - temporary keep-awake => never timeout
 * - else => default timeout
 */
uint32_t screen_timeout_get_effective_ms(void)
{
    if (s_always_on) return 0;
    if (keep_awake_active()) return 0;
    return s_default_ms;
}

static void recompute_deadline(uint32_t now_ms)
{
    uint32_t ms = screen_timeout_get_effective_ms();

    if (ms == 0) {
        s_deadline_valid = false;
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

    s_keep_awake_refs = 0;

    s_deadline_valid = false;
    s_deadline_ms = 0;

    screen_timeout_mark_activity();
}

void screen_timeout_mark_activity(void)
{
    recompute_deadline(now_ms_local());
}

bool screen_timeout_expired(uint32_t now_ms)
{
    if (!s_deadline_valid) return false;
    return (now_ms >= s_deadline_ms);
}

uint32_t screen_timeout_deadline_ms(void)
{
    return s_deadline_valid ? s_deadline_ms : 0;
}

/* -------- NEW: keep-awake control -------- */

void screen_keep_awake_set(bool on)
{
    if (on) {
        if (s_keep_awake_refs == 0) {
            s_keep_awake_refs = 1;
            screen_timeout_mark_activity();
            ESP_LOGI(TAG, "KeepAwake -> ON (refs=%u)", (unsigned)s_keep_awake_refs);
        }
    } else {
        if (s_keep_awake_refs != 0) {
            s_keep_awake_refs = 0;
            screen_timeout_mark_activity();
            ESP_LOGI(TAG, "KeepAwake -> OFF");
        }
    }
}

bool screen_keep_awake_get(void)
{
    return keep_awake_active();
}

bool screen_keep_awake_toggle(void)
{
    bool now_on = !keep_awake_active();
    screen_keep_awake_set(now_on);
    return now_on;
}

void screen_keep_awake_acquire(void)
{
    if (s_keep_awake_refs == 0) {
        s_keep_awake_refs = 1;
        screen_timeout_mark_activity();
        ESP_LOGI(TAG, "KeepAwake acquire (refs=%u)", (unsigned)s_keep_awake_refs);
        return;
    }

    if (s_keep_awake_refs < 0xFFFFFFFFu) s_keep_awake_refs++;
    // no need to mark activity; still never-timeout
    ESP_LOGI(TAG, "KeepAwake acquire (refs=%u)", (unsigned)s_keep_awake_refs);
}

void screen_keep_awake_release(void)
{
    if (s_keep_awake_refs == 0) return;

    s_keep_awake_refs--;
    ESP_LOGI(TAG, "KeepAwake release (refs=%u)", (unsigned)s_keep_awake_refs);

    if (s_keep_awake_refs == 0) {
        // when keep-awake ends, start deadline from "now"
        screen_timeout_mark_activity();
        ESP_LOGI(TAG, "KeepAwake -> OFF (refs=0)");
    }
}

uint32_t screen_keep_awake_refcount(void)
{
    return s_keep_awake_refs;
}
