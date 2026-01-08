#include "watch_logbuf.h"
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LOGBUF_SZ (16 * 1024)

static char s_buf[LOGBUF_SZ];
static size_t s_head = 0;
static bool s_wrapped = false;

static SemaphoreHandle_t s_mu;

/* NEW */
static uint32_t s_seq = 0;

void watch_logbuf_init(void)
{
    s_mu = xSemaphoreCreateMutex();
}

/* NEW */
uint32_t watch_logbuf_seq(void)
{
    return s_seq;
}

void watch_logbuf_write(const char *s, size_t len)
{
    if (!s || !len) return;
    if (!s_mu) return;

    xSemaphoreTake(s_mu, portMAX_DELAY);

    for (size_t i = 0; i < len; i++) {
        s_buf[s_head++] = s[i];
        if (s_head >= LOGBUF_SZ) {
            s_head = 0;
            s_wrapped = true;
        }
    }

    /* NEW: bump once per write batch */
    s_seq++;

    xSemaphoreGive(s_mu);
}

size_t watch_logbuf_snapshot(char *out, size_t out_sz)
{
    if (!out || out_sz < 2) return 0;
    if (!s_mu) { out[0] = 0; return 0; }

    xSemaphoreTake(s_mu, portMAX_DELAY);

    size_t n = 0;

    if (!s_wrapped) {
        size_t to_copy = s_head;
        if (to_copy > out_sz - 1) to_copy = out_sz - 1;
        memcpy(out, s_buf, to_copy);
        n = to_copy;
    } else {
        size_t first = LOGBUF_SZ - s_head;
        size_t second = s_head;

        size_t c1 = first;
        if (c1 > out_sz - 1) c1 = out_sz - 1;
        memcpy(out, &s_buf[s_head], c1);
        n += c1;

        if (n < out_sz - 1) {
            size_t c2 = second;
            if (c2 > (out_sz - 1 - n)) c2 = (out_sz - 1 - n);
            memcpy(out + n, s_buf, c2);
            n += c2;
        }
    }

    out[n] = 0;

    xSemaphoreGive(s_mu);
    return n;
}
