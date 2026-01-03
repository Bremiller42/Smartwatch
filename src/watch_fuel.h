#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Latest readings (updated by task)
extern int   g_watch_batt_pct;     // 0..100, -1 unknown
extern float g_watch_batt_v;       // volts, <0 unknown

esp_err_t watch_fuel_init(void);   // calls watch_i2c_init() and probes 0x36
void      watch_fuel_start_task(void);

// Optional: read on demand
esp_err_t watch_fuel_read_soc(float *pct_out);
esp_err_t watch_fuel_read_vcell(float *v_out);

#ifdef __cplusplus
}
#endif
