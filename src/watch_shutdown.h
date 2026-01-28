#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SHDN_OK = 0,
    SHDN_LOW,
    SHDN_CRITICAL,
    SHDN_SHUTTING_DOWN,
} shdn_state_t;

esp_err_t    watch_shutdown_init(void);
void         watch_shutdown_update(float vbat, bool screen_awake);
shdn_state_t watch_shutdown_state(void);

// NEW: lets other modules know shutdown controller is ready (prevents early-boot issues)
bool         watch_shutdown_is_ready(void);

// NEW: "critical UI latch" (survives deep sleep; no NVS writes)
void         watch_shutdown_set_low_power_latch(bool on);
bool         watch_shutdown_low_power_latched(void);
float watch_shutdown_get_critical_v(void);
bool watch_shutdown_low_power_latched(void);
void watch_shutdown_set_low_power_latch(bool on);
bool watch_shutdown_storage_allowed(void);

#ifdef __cplusplus
}
#endif
