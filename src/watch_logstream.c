// FILE: src/watch_logstream.c
#include "watch_logstream.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

#include "esp_log.h"

// -------- Tune --------
#define LOG_RB_BYTES     (16 * 1024)   // ringbuffer capacity
#define LOG_LINE_MAX     128           // per-line formatting buffer (stack)
#define LOG_READ_MAX     512           // max bytes pulled per read()
#define LOG_PURGE_ON_FULL      1
#define LOG_PURGE_DRAIN_BYTES  (8 * 1024)   // how much to try draining when full

static RingbufHandle_t s_rb = NULL;
static vprintf_like_t s_orig_vprintf = NULL;

static uint32_t s_drop = 0;
static uint32_t s_written = 0;
static void logstream_purge_some(size_t bytes_to_purge)
{
    if (!s_rb) return;

    size_t total = 0;
    while (total < bytes_to_purge) {
        size_t item_sz = 0;
        uint8_t *p = (uint8_t *)xRingbufferReceiveUpTo(s_rb, &item_sz, 0, 256);
        if (!p || item_sz == 0) break;
        total += item_sz;
        vRingbufferReturnItem(s_rb, p);
    }
}
static size_t strip_ansi(char *s)
{
    char *d = s;
    for (char *p = s; *p; ) {
        if (p[0] == '\x1b' && p[1] == '[') {
            // skip until 'm' or end
            p += 2;
            while (*p && *p != 'm') p++;
            if (*p == 'm') p++;
        } else {
            *d++ = *p++;
        }
    }
    *d = '\0';
    return (size_t)(d - s);
}
static size_t sanitize_ascii(char *s)
{
    char *d = s;
    for (char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;

        // allow newline + carriage return (we’ll normalize CR below)
        if (c == '\n' || c == '\r' || c == '\t') {
            *d++ = (char)c;
            continue;
        }

        // keep printable ASCII only
        if (c >= 32 && c <= 126) {
            *d++ = (char)c;
        } else {
            // drop anything else
        }
    }
    *d = '\0';
    return (size_t)(d - s);
}

/* Optional: cheap filters */
static inline bool skip_mirror_by_task(void)
{
    const char *tn = pcTaskGetName(NULL);
    if (!tn) return false;

    // be conservative: never mirror from nimble / bt / host tasks
    if (strstr(tn, "nimble")) return true;
    if (strstr(tn, "host"))   return true;
    if (strstr(tn, "bt"))     return true;

    return false;
}

static inline bool skip_mirror_fast_by_fmt(const char *fmt)
{
    if (strstr(fmt, "LVGL")) return true;
    if (strstr(fmt, "heap")) return true;
    return false;
}

static inline bool should_skip_line(const char *line)
{
    if (strstr(line, "LVGL")) return true;
    if (strstr(line, "heap")) return true;
    if (strstr(line, "wifi")) return true;
    if (strstr(line, "RX chunk")) return true;
    return false;
}

static int log_vprintf_hook(const char *fmt, va_list args)
{
    // IMPORTANT: copy BEFORE consuming
    va_list args_print;
    va_copy(args_print, args);

    int ret = (s_orig_vprintf) ? s_orig_vprintf(fmt, args_print) : vprintf(fmt, args_print);
    va_end(args_print);
    // NEVER do RTOS/ringbuffer work from ISR context
    if (xPortInIsrContext()) return ret;

    // Mirror decisions (fast)
    if (!s_rb) return ret;
    if (skip_mirror_by_task()) return ret;
    if (skip_mirror_fast_by_fmt(fmt)) return ret;

    // Format once for mirror
    va_list args_m;
    va_copy(args_m, args);

    char line[LOG_LINE_MAX];
    int n = vsnprintf(line, sizeof(line), fmt, args_m);
    va_end(args_m);

    if (n <= 0) return ret;
    line[sizeof(line) - 1] = 0;

    // REMOVE ANSI COLOR CODES
    strip_ansi(line);
    sanitize_ascii(line);

    if (should_skip_line(line)) return ret;


    // Ensure newline for nicer UI streaming
    size_t len = strnlen(line, sizeof(line));
    bool has_nl = (len > 0 && line[len - 1] == '\n');

    // Non-blocking send; if full, drop
    if (xRingbufferSend(s_rb, line, len, 0) != pdTRUE) {
        s_drop++;

    #if LOG_PURGE_ON_FULL
        // Drop old logs to recover space, then retry once
        logstream_purge_some(LOG_PURGE_DRAIN_BYTES);

        if (xRingbufferSend(s_rb, line, len, 0) != pdTRUE) {
            // still full; drop for real
            return ret;
        }
    #else
        return ret;
    #endif
    }

    s_written += (uint32_t)len;

    if (!has_nl) {
        const char nl = '\n';
        (void)xRingbufferSend(s_rb, &nl, 1, 0); // if it fails, whatever
        s_written += 1;
    }
    static uint32_t s_last_purge_mark = 0;
    if ((s_drop - s_last_purge_mark) >= 50) {
        const char *m = "[logstream] PURGE (ringbuffer was full)\n";
        (void)xRingbufferSend(s_rb, m, strlen(m), 0);
        s_last_purge_mark = s_drop;
    }

    return ret;
}

void watch_logstream_init(void)
{
    if (s_rb) return;

    s_rb = xRingbufferCreate(LOG_RB_BYTES, RINGBUF_TYPE_BYTEBUF);
    // If this fails, we still keep normal UART logs
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf_hook);
}

size_t watch_logstream_read(char *dst, size_t dst_sz)
{
    if (!dst || dst_sz < 2 || !s_rb) return 0;

    // Pull up to dst_sz-1 bytes in one go (bytebuf can return any chunk size)
    size_t item_sz = 0;
    uint8_t *p = (uint8_t *)xRingbufferReceiveUpTo(s_rb, &item_sz, 0, dst_sz - 1);
    if (!p || item_sz == 0) return 0;

    memcpy(dst, p, item_sz);
    vRingbufferReturnItem(s_rb, p);

    dst[item_sz] = 0;
    return item_sz;
}

uint32_t watch_logstream_dropped(void) { return s_drop; }
uint32_t watch_logstream_written(void) { return s_written; }
