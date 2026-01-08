#pragma once
#include <stdint.h>
#include "lvgl.h"     // <-- add this

#ifdef __cplusplus
extern "C" {
#endif

// Settings-exposed knobs
extern uint32_t g_wifi_off_delay_ms;

void sleep_system_init(void);
void mark_user_activity(void);
void activity_event_cb(lv_event_t *e);

#ifdef __cplusplus
}
#endif
