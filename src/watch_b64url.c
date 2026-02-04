// FILE: src/watch_b64url.c
#include "watch_b64url.h"
#include "mbedtls/base64.h"
#include "esp_err.h"

#include <string.h>

static void str0(char *dst, size_t n) { if (dst && n) dst[0] = 0; }

esp_err_t b64url_decode_to(char *dst, size_t dst_len, const char *src)
{
    str0(dst, dst_len);
    if (!dst || dst_len == 0 || !src) return ESP_ERR_INVALID_ARG;

    // Convert URL-safe -> standard and pad to multiple of 4.
    // We'll use a small temp buffer; limit src length to something sane.
    size_t sl = strlen(src);
    if (sl > 2048) return ESP_ERR_INVALID_SIZE;

    char tmp[2100];
    memcpy(tmp, src, sl);
    tmp[sl] = 0;

    for (size_t i = 0; i < sl; i++) {
        if (tmp[i] == '-') tmp[i] = '+';
        else if (tmp[i] == '_') tmp[i] = '/';
    }

    // Add padding '=' if missing
    size_t mod = sl % 4;
    if (mod != 0) {
        size_t pad = 4 - mod;
        if (sl + pad + 1 >= sizeof(tmp)) return ESP_ERR_INVALID_SIZE;
        for (size_t i = 0; i < pad; i++) tmp[sl + i] = '=';
        sl += pad;
        tmp[sl] = 0;
    }

    unsigned char out[2048];
    size_t out_len = 0;

    int rc = mbedtls_base64_decode(out, sizeof(out), &out_len,
                                  (const unsigned char*)tmp, sl);
    if (rc != 0) {
        str0(dst, dst_len);
        return ESP_FAIL;
    }

    // Copy into dst with truncation
    size_t n = (out_len < (dst_len - 1)) ? out_len : (dst_len - 1);
    memcpy(dst, out, n);
    dst[n] = 0;
    return ESP_OK;
}
