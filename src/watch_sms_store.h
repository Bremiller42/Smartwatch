// FILE: src/watch_sms_store.h
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t msg_id;
    uint64_t ts_ms;
    char thread_id[128];
    char sender[96];
    char body[512];     // preview/truncated
    char pkg[96];
} watch_sms_latest_t;

/** Append a message to SD (/sdcard/sms/messages.log) and update latest (/sdcard/sms/latest.txt) */
esp_err_t watch_sms_store_ingest(uint32_t msg_id,
                                uint64_t ts_ms,
                                const char *thread_id,
                                const char *sender,
                                const char *body,
                                const char *pkg);

/** Read latest snapshot (RAM). Thread-safe enough for UI read-copy */
bool watch_sms_get_latest(watch_sms_latest_t *out);

#ifdef __cplusplus
}
#endif
