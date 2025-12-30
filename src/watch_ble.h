#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ble_rx_cb_t)(const char *msg, int len);

/**
 * Init BLE + start advertising.
 * Safe to call once at boot.
 */
esp_err_t ble_init(ble_rx_cb_t on_rx);

/** Start advertising (if stopped) */
void ble_start(void);

/** Stop advertising + disconnect if connected */
void ble_stop(void);

/** True if we currently have a BLE connection */
bool ble_is_connected(void);

/**
 * Optional: send a notify out on TX characteristic (if phone subscribed).
 * Returns ESP_OK if queued, otherwise error.
 */
esp_err_t ble_notify_tx(const char *msg);

#ifdef __cplusplus
}
#endif
