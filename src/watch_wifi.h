#pragma once
#include "esp_err.h"

/* core wifi */
void wifi_ensure_started(void);
esp_err_t wifi_start_sta(const char *ssid, const char *pass);
void wifi_stop(void);
int  wifi_get_rssi_dbm(int *out_rssi);

/* scanning */
void start_wifi_scan(void);
void wifi_forget_saved(void);
