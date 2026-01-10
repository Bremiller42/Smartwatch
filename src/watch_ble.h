#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ble_rx_cb_t)(const char *msg, int len);

esp_err_t ble_init(ble_rx_cb_t on_rx);

void ble_start(void);
void ble_stop(void);

bool ble_is_connected(void);

void ble_ui_mark_dirty_from_ble_thread(void);
bool ble_ui_take_dirty(void);

esp_err_t ble_notify_tx(const char *msg);

void ble_request_sleep_params(void);
void ble_request_awake_params(void);

/* NEW: explicit enable/disable API (user intent) */
void ble_set_enabled(bool on);
bool ble_is_enabled(void);

#ifdef __cplusplus
}
#endif
