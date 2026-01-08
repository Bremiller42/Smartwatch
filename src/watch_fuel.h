#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int   g_watch_batt_pct;
extern float g_watch_batt_v;

esp_err_t watch_fuel_init(void);
esp_err_t watch_fuel_read_vcell(float *v_out);
esp_err_t watch_fuel_read_soc(float *pct_out);

void watch_fuel_start_task(void);

#ifdef __cplusplus
}
#endif
