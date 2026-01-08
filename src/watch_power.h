#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t watch_power_init(void);
esp_err_t watch_power_set_profile_awake(void);
esp_err_t watch_power_set_profile_sleep(void);

#ifdef __cplusplus
}
#endif
