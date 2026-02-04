// FILE: src/watch_b64url.h
#pragma once
#include <stddef.h>
#include "esp_err.h"

/**
 * Decode URL-safe Base64 (Android URL_SAFE, NO_WRAP) into dst.
 * dst is always NUL-terminated if dst_len > 0.
 * Returns ESP_OK on success.
 */
esp_err_t b64url_decode_to(char *dst, size_t dst_len, const char *src);
