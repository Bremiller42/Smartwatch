// watch_shutdown.h
#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Define the state type FIRST
typedef enum {
    SHDN_OK = 0,
    SHDN_LOW,
    SHDN_CRITICAL,
    SHDN_SHUTTING_DOWN,
} shdn_state_t;

// Public API
esp_err_t     watch_shutdown_init(void);
void          watch_shutdown_update(float vbat, bool screen_awake);
shdn_state_t  watch_shutdown_state(void);
