#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SLP_AWAKE = 0,
    SLP_SCREEN_OFF,     // backlight off, UI blanked, touch polling reduced
    SLP_WIFI_OFF,       // WiFi stopped
    SLP_BLE_SLOW,       // BLE stays connected but params slowed
    SLP_BLE_OFF,        // BLE fully off (optional)
} sleep_stage_t;

/* Stage is owned/defined once in watch_globals.c */
extern volatile sleep_stage_t g_sleep_stage;

/* Settings-exposed knobs (owned in watch_sleep.c or globals, depending on your design) */
extern uint32_t g_wifi_off_delay_ms;
extern uint32_t g_ble_slow_delay_ms;
extern uint32_t g_ble_off_delay_ms;

/* g_screen_timeout_ms lives in watch_globals.c per your globals paste */
extern uint32_t g_screen_timeout_ms;

/* --------- New stable API --------- */
esp_err_t watch_sleep_init(void);
void watch_sleep_notify_activity(void);
void watch_sleep_force_awake(void);
void watch_sleep_force_screen_off(void);

/* --------- Backward-compat wrappers ---------
   These keep older code compiling without you hunting every callsite. */
static inline void sleep_system_init(void) { (void)watch_sleep_init(); }
static inline void mark_user_activity(void) { watch_sleep_notify_activity(); }

/* Use this in LVGL input events etc. */
static inline void activity_event_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);

    switch (c) {
        case LV_EVENT_PRESSED:
        case LV_EVENT_PRESSING:
        case LV_EVENT_RELEASED:
        case LV_EVENT_CLICKED:
        case LV_EVENT_LONG_PRESSED:
        case LV_EVENT_LONG_PRESSED_REPEAT:
        case LV_EVENT_GESTURE:
        case LV_EVENT_KEY:
        case LV_EVENT_VALUE_CHANGED:
        case LV_EVENT_FOCUSED:
            watch_sleep_notify_activity();
            break;

        default:
            break; // ignore draw/layout/refresh/etc.
    }
}

// Called from BSP touch ISR to hint the sleep manager
void watch_sleep_touch_irq_hint_from_isr(void);

#ifdef __cplusplus
}
#endif
