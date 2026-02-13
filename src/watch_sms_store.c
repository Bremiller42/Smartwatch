// FILE: src/watch_sms_store.c
#include "watch_sms_store.h"
#include "watch_sdcard.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

static const char *TAG = "SMS_STORE";

static watch_sms_latest_t s_latest;
static bool s_latest_valid = false;

static void strlcpy0(char *dst, const char *src, size_t dstsz)
{
    if (!dst || dstsz == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= dstsz) n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

// Keep only printable ASCII. Drop all UTF-8 multibyte + control chars.
// Also ensures no '|' remains (since your log is pipe-delimited).
static void sms_sanitize_ascii(char *dst, size_t dstsz, const char *src)
{
    if (!dst || dstsz == 0) return;
    dst[0] = '\0';
    if (!src) return;

    size_t w = 0;
    for (size_t i = 0; src[i] && w < dstsz - 1; i++) {
        unsigned char c = (unsigned char)src[i];

        // normalize whitespace
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';

        // keep printable ASCII only
        if (c >= 0x20 && c <= 0x7E) {
            if (c == '|') c = ' ';     // never allow delimiter in fields
            dst[w++] = (char)c;
        } else {
            // Drop non-ASCII byte (this will strip emoji/curly quotes/etc.)
            // If you prefer visible replacement, use: dst[w++] = '?';
        }
    }

    // trim trailing spaces
    while (w > 0 && dst[w - 1] == ' ') w--;
    dst[w] = '\0';
}


static void ensure_dir(const char *path)
{
    // best-effort
    mkdir(path, 0775);
}

typedef struct {
    uint32_t msg_id;
    uint64_t ts_ms;
    const char *thread_id;
    const char *sender;
    const char *body;
    const char *pkg;
} sms_write_ctx_t;

static esp_err_t sms_write_work(void *p)
{
    sms_write_ctx_t *c = (sms_write_ctx_t*)p;
    if (!c) return ESP_ERR_INVALID_ARG;

    ensure_dir("/sdcard/sms");

    // 1) append-only log line (Base64-safe payload on Android, but here we store decoded fields as a single line)
    // Format: msg_id|ts_ms|thread_id|sender|pkg|body\n
    // NOTE: body can contain '\n' after decode, so we sanitize to spaces for log readability.
    char body_sane[1024];
    strlcpy0(body_sane, c->body, sizeof(body_sane));
    for (char *q = body_sane; *q; q++) {
        if (*q == '\r' || *q == '\n') *q = ' ';
    }

    FILE *f = fopen("/sdcard/sms/messages.log", "ab");
    if (!f) {
        ESP_LOGE(TAG, "fopen messages.log failed");
        return ESP_FAIL;
    }

    // Keep it simple: pipe-separated, no escaping, since we already sanitized \n and you can avoid '|' on Android
    // But some senders/bodies may still contain '|', so we also replace it here.
    for (char *q = body_sane; *q; q++) if (*q == '|') *q = ' ';

    char sender_sane[128]; strlcpy0(sender_sane, c->sender, sizeof(sender_sane));
    for (char *q = sender_sane; *q; q++) if (*q == '|') *q = ' ';

    char thread_sane[192]; strlcpy0(thread_sane, c->thread_id, sizeof(thread_sane));
    for (char *q = thread_sane; *q; q++) if (*q == '|') *q = ' ';

    char pkg_sane[128]; strlcpy0(pkg_sane, c->pkg, sizeof(pkg_sane));
    for (char *q = pkg_sane; *q; q++) if (*q == '|') *q = ' ';

    fprintf(f, "%u|%llu|%s|%s|%s|%s\n",
            (unsigned)c->msg_id,
            (unsigned long long)c->ts_ms,
            thread_sane,
            sender_sane,
            pkg_sane,
            body_sane);

    fclose(f);

    // 2) latest snapshot file (small, easy to load on boot later)
    FILE *g = fopen("/sdcard/sms/latest.txt", "wb");
    if (g) {
        fprintf(g, "%u|%llu|%s|%s|%s|%s\n",
                (unsigned)c->msg_id,
                (unsigned long long)c->ts_ms,
                thread_sane,
                sender_sane,
                pkg_sane,
                body_sane);
        fclose(g);
    }

    return ESP_OK;
}

esp_err_t watch_sms_store_ingest(uint32_t msg_id,
                                uint64_t ts_ms,
                                const char *thread_id,
                                const char *sender,
                                const char *body,
                                const char *pkg)
{
    // Sanitize all incoming decoded strings (ASCII-only)
    static char thread_s[192];
    static char sender_s[128];
    static char body_s[1024];
    static char pkg_s[128];

    sms_sanitize_ascii(thread_s, sizeof(thread_s), thread_id);
    sms_sanitize_ascii(sender_s, sizeof(sender_s), sender);
    sms_sanitize_ascii(pkg_s,    sizeof(pkg_s),    pkg);
    sms_sanitize_ascii(body_s,   sizeof(body_s),   body);

    // Update RAM latest (preview)
    s_latest.msg_id = msg_id;
    s_latest.ts_ms  = ts_ms;
    strlcpy0(s_latest.thread_id, thread_s, sizeof(s_latest.thread_id));
    strlcpy0(s_latest.sender,    sender_s, sizeof(s_latest.sender));
    strlcpy0(s_latest.pkg,       pkg_s,    sizeof(s_latest.pkg));

    // Keep body preview bounded
    char preview[512];
    strlcpy0(preview, body_s, sizeof(preview));
    s_latest_valid = true;
    strlcpy0(s_latest.body, preview, sizeof(s_latest.body));

    // Write to SD in one mount (use sanitized pointers!)
    sms_write_ctx_t ctx = {
        .msg_id = msg_id,
        .ts_ms = ts_ms,
        .thread_id = thread_s,
        .sender = sender_s,
        .body = body_s,
        .pkg = pkg_s
    };
    return watch_sdcard_do(sms_write_work, &ctx, 5000);
}


bool watch_sms_get_latest(watch_sms_latest_t *out)
{
    if (!out || !s_latest_valid) return false;
    *out = s_latest; // struct copy
    return true;
}
