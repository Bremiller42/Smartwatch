#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stdio.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Mount the SD card at /sdcard using SPI (SDSPI mode)
 *
 * @return
 *  - ESP_OK on success
 *  - ESP_FAIL or other esp_err_t on failure (no card, wiring, SPI conflict, etc.)
 */
esp_err_t watch_sdcard_mount(void);

/**
 * @brief Unmount SD card and free resources
 *
 * Safe to call even if not mounted.
 */
void watch_sdcard_unmount(void);

/**
 * @brief Callback signature used by sdcard_do().
 * Return ESP_OK on success; any error aborts and returns upward.
 */
typedef esp_err_t (*watch_sd_work_fn_t)(void *ctx);

/**
 * @brief Mount SD, run callback, unmount SD.
 *
 * Thread-safe. If another task is already doing SD work, this will
 * block until it finishes (up to timeout_ms).
 *
 * @param work         Callback that does the SD work (open/read/write/etc).
 * @param ctx          User context passed to callback.
 * @param timeout_ms   How long to wait for the SD "lock" (0 = no wait).
 */
esp_err_t watch_sdcard_do(watch_sd_work_fn_t work, void *ctx, uint32_t timeout_ms);

/**
 * @brief Convenience helper: write a buffer to a file on /sdcard
 *
 * @param path        Absolute path like "/sdcard/log.txt"
 * @param data        Buffer to write
 * @param len         Bytes to write
 * @param append      true = append, false = overwrite
 */
esp_err_t watch_sdcard_write_file(const char *path, const void *data, size_t len, bool append, uint32_t timeout_ms);

/**
 * @brief Convenience helper: read entire file into a malloc'd buffer.
 *
 * Caller owns (*out_buf) and must free().
 *
 * @param path        Absolute path like "/sdcard/config.bin"
 * @param out_buf     malloc'd buffer returned here
 * @param out_len     length returned here
 * @param max_bytes   safety cap (0 = no cap). If file bigger, returns ESP_ERR_INVALID_SIZE.
 */
esp_err_t watch_sdcard_read_file(const char *path, uint8_t **out_buf, size_t *out_len, size_t max_bytes, uint32_t timeout_ms);

/**
 * @brief Convenience helper: append a line (adds '\n' if not present).
 */
esp_err_t watch_sdcard_append_line(const char *path, const char *line, uint32_t timeout_ms);
esp_err_t watch_sdcard_self_test(uint32_t timeout_ms);
void watch_sdcard_init(void);
typedef esp_err_t (*watch_sd_work_fn_t)(void *ctx);
bool watch_sdcard_is_mounted(void);
const char *watch_sdcard_mount_point(void);
esp_err_t watch_sdcard_get_volume_label(char *out, size_t out_sz);
const char *watch_sdcard_volume_label_cached(void);
const char *watch_sdcard_id_cached(void);
// Cached, safe-to-call-from-UI getters
const char *watch_sdcard_id_cached(void);

uint64_t watch_sdcard_total_kb_cached(void);
uint64_t watch_sdcard_free_kb_cached(void);

#ifdef __cplusplus
}
#endif