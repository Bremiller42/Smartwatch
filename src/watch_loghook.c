#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "watch_logbuf.h"

static vprintf_like_t s_orig_vprintf = NULL;

static uint32_t fnv1a32(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static inline bool skip_mirror_by_task(void)
{
    const char *tn = pcTaskGetName(NULL);
    return (tn && strcmp(tn, "nimble_host") == 0);
}

static inline bool skip_mirror_fast_by_fmt(const char *fmt)
{
    // Cheap “pre-filter” on the format string to avoid vsnprintf work
    if (strstr(fmt, "LVGL")) return true;
    if (strstr(fmt, "heap")) return true;
    // If you want, also skip BLE spam by fmt/tag:
    // if (strstr(fmt, "BLE")) return true;
    return false;
}

static inline bool should_skip_line(const char *line)
{
    // Filter on rendered line
    if (strstr(line, "LVGL")) return true;
    if (strstr(line, "heap")) return true;
    if (strstr(line, "wifi")) return true;

    // The killer spam line:
    if (strstr(line, "RX chunk")) return true;

    return false;
}

static int log_vprintf(const char *fmt, va_list args)
{
    // 1) Print to UART first (original behavior)
    int ret = (s_orig_vprintf) ? s_orig_vprintf(fmt, args) : vprintf(fmt, args);

    // 2) Decide if we will MIRROR (copy to internal buffer)
    if (skip_mirror_by_task()) return ret;          // <-- protects nimble_host stack
    if (skip_mirror_fast_by_fmt(fmt)) return ret;   // <-- avoids formatting cost

    // 3) Only now format a small line for mirroring
    char tmp[96];  // smaller than 128 = less stack
    va_list ac;
    va_copy(ac, args);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ac);
    va_end(ac);

    if (n > 0) {
        tmp[sizeof(tmp) - 1] = 0;

        if (!should_skip_line(tmp)) {
            static uint32_t last_h = 0;
            uint32_t h = fnv1a32(tmp);
            if (h != last_h) {
                last_h = h;
                size_t len = (n < (int)sizeof(tmp)) ? (size_t)n : (sizeof(tmp) - 1);
                watch_logbuf_write(tmp, len);
            }
        }
    }

    return ret;
}

void watch_loghook_init(void)
{
    watch_logbuf_init();
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf);
}
