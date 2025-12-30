#pragma once
#include <lvgl.h>

/* init timers + sleep system */
void sleep_system_init(void);

/* shared activity function used in UI events */
void mark_user_activity(void);

/* event cb attached to screens */
void activity_event_cb(lv_event_t *e);
