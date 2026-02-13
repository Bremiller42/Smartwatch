// FILE: watch_ble.c

#include "watch_ble.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lvgl.h"

#include "watch_audio.h"
#include "watch_globals.h"
#include "ui_priv.h"

// NimBLE
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_sm.h"
#include "watch_sms_store.h"
#include "watch_b64url.h"

#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "store/config/ble_store_config.h"

#include "freertos/queue.h"

static const char *BLE_TAG = "BLE";

/* ---------------- Internal State ---------------- */
static ble_rx_cb_t s_on_rx = NULL;

static uint16_t s_conn_handle     = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_val_handle   = 0;
static bool     s_notify_enabled  = false;
static uint8_t  s_own_addr_type   = BLE_OWN_ADDR_PUBLIC;

static void ble_advertise(void);
static void ble_stop_adv_only(void);

static volatile bool s_ble_ui_dirty = false;
static volatile bool s_ble_enabled = true;  // mirrors user intent (default on)

/* RX framing */
static char   s_rx_accum[1024];
static size_t s_rx_len = 0;

/* Work buffer for tokenization */
static char s_work[1024];

#ifdef __cplusplus
extern "C" {
#endif
void ble_store_config_init(void);
#ifdef __cplusplus
}
#endif

/* ---------------- TX queue (notify from safe context) ---------------- */
#define BLE_TXQ_DEPTH      16
#define BLE_TX_MAX_BYTES   256

typedef struct {
    uint16_t conn;
    uint16_t val_handle;
    uint16_t len;
    char     data[BLE_TX_MAX_BYTES];
} ble_tx_item_t;

static QueueHandle_t s_txq = NULL;
static TaskHandle_t  s_tx_task = NULL;

static void ble_txq_task(void *arg)
{
    (void)arg;
    ble_tx_item_t it;

    for (;;) {
        if (xQueueReceive(s_txq, &it, portMAX_DELAY) == pdTRUE) {

            if (!s_notify_enabled) continue;
            if (it.conn == BLE_HS_CONN_HANDLE_NONE || it.val_handle == 0) continue;

            struct os_mbuf *om = ble_hs_mbuf_from_flat(it.data, it.len);
            if (!om) continue;

            int rc = ble_gatts_notify_custom(it.conn, it.val_handle, om);
            if (rc != 0) {
                os_mbuf_free_chain(om);
            }
        }
    }
}

static inline void ble_txq_send_str(const char *s)
{
    if (!s || !s_txq) return;
    if (!s_notify_enabled) return;
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_tx_val_handle == 0) return;

    ble_tx_item_t it = {0};
    it.conn = s_conn_handle;
    it.val_handle = s_tx_val_handle;

    size_t n = strlen(s);
    if (n >= BLE_TX_MAX_BYTES) n = BLE_TX_MAX_BYTES - 1;
    memcpy(it.data, s, n);
    it.data[n] = '\0';
    it.len = (uint16_t)n;

    (void)xQueueSend(s_txq, &it, 0); // non-blocking drop if full
}

/* ---------------- UUIDs ---------------- */
static const ble_uuid128_t UUID_SVC_NOTIF = BLE_UUID128_INIT(
    0x2d,0x1a,0x9b,0x24,0x9c,0xb1,0x4e,0x9c,0x9c,0x1d,0x37,0x3a,0x7e,0x8f,0x4a,0x10);

static const ble_uuid128_t UUID_CHR_RX = BLE_UUID128_INIT(
    0x2d,0x1a,0x9b,0x24,0x9c,0xb1,0x4e,0x9c,0x9c,0x1d,0x37,0x3a,0x7e,0x8f,0x4a,0x11);

static const ble_uuid128_t UUID_CHR_TX = BLE_UUID128_INIT(
    0x2d,0x1a,0x9b,0x24,0x9c,0xb1,0x4e,0x9c,0x9c,0x1d,0x37,0x3a,0x7e,0x8f,0x4a,0x12);

/* ---------------- Notification state table ---------------- */
#define NOTIF_MAX_ITEMS 30
#define NOTIF_ID_MAX    256

typedef struct {
    bool used;
    char id[NOTIF_ID_MAX];
    notif_type_t group;
} notif_item_t;

static notif_item_t s_notifs[NOTIF_MAX_ITEMS];
static bool s_notif_snapshot_mode = false;

static int notif_find(const char *id)
{
    for (int i = 0; i < NOTIF_MAX_ITEMS; i++) {
        if (s_notifs[i].used && strncmp(s_notifs[i].id, id, NOTIF_ID_MAX) == 0) return i;
    }
    return -1;
}

static int notif_free_slot(void)
{
    for (int i = 0; i < NOTIF_MAX_ITEMS; i++) {
        if (!s_notifs[i].used) return i;
    }
    return -1;
}

static void ui_hide_phone_batt_cb(void *arg)
{
    (void)arg;
    if (!phone_batt_lbl) return;
    lv_label_set_text(phone_batt_lbl, "");
    lv_obj_add_flag(phone_batt_lbl, LV_OBJ_FLAG_HIDDEN);
}

static void phone_batt_clear_local(void)
{
    g_phone_batt_pct = -1;
    g_phone_batt_charging = false;
    lv_async_call(ui_hide_phone_batt_cb, NULL);
}

static void notif_clear_all_local(void)
{
    memset(s_notifs, 0, sizeof(s_notifs));
    for (int i = 0; i < NG_MAX; i++) g_notif_counts[i] = 0;
    ui_notif_refresh_async();
}

static void notif_recalc_counts(void)
{
    for (int i = 0; i < NG_MAX; i++) g_notif_counts[i] = 0;

    for (int i = 0; i < NOTIF_MAX_ITEMS; i++) {
        if (!s_notifs[i].used) continue;
        notif_type_t g = s_notifs[i].group;
        if (g >= 0 && g < NG_MAX && g_notif_counts[g] < 999) g_notif_counts[g]++;
    }
}

/* ---------------- Small UI helpers ---------------- */
static void ble_ui_mark_dirty(void) { s_ble_ui_dirty = true; }

void ble_ui_mark_dirty_from_ble_thread(void) { s_ble_ui_dirty = true; }

bool ble_ui_take_dirty(void)
{
    if (!s_ble_ui_dirty) return false;
    s_ble_ui_dirty = false;
    return true;
}

/* ---------------- Store status (bonding) ---------------- */
static int ble_store_status_cb(struct ble_store_status_event *event, void *arg)
{
    (void)event;
    (void)arg;
    return 0;
}

/* ---------------- Group mapping ---------------- */
notif_type_t notif_group_from_pkg_and_type(const char *type, const char *pkg)
{
    if (pkg && *pkg) {
        if (strstr(pkg, "com.google.android.youtube")) return NG_YOUTUBE;
        if (strstr(pkg, "com.reddit.frontpage"))       return NG_REDDIT;
        if (strstr(pkg, "com.facebook.orca"))          return NG_MESSENGER;
        if (strstr(pkg, "discord"))                    return NG_DISCORD;
        if (strstr(pkg, "amazon"))                     return NG_AMAZON;
        if (strstr(pkg, "com.textra"))                 return NG_SMS;
        if (strstr(pkg, "googlequicksearchbox"))       return NG_WEATHER;
    }

    if (type) {
        if (!strcmp(type, "SMS"))     return NG_SMS;
        if (!strcmp(type, "EMAIL"))   return NG_EMAIL;
        if (!strcmp(type, "SYS"))     return NG_SYSTEM;
        if (!strcmp(type, "WEATHER")) return NG_WEATHER;
    }

    return NG_APP;
}

/* ---------------- Battery parsing ---------------- */
// Frame: "B|P|<pct>|C|<0|1>"
static bool parse_batt_frame(const char *frame)
{
    if (!frame) return false;
    if (!(frame[0] == 'B' && frame[1] == '|')) return false;

    char work[64];
    strncpy(work, frame, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    char *save = NULL;
    (void)strtok_r(work, "|", &save);          // B
    char *p_tag = strtok_r(NULL, "|", &save);  // P
    char *p_val = strtok_r(NULL, "|", &save);  // pct
    char *c_tag = strtok_r(NULL, "|", &save);  // C
    char *c_val = strtok_r(NULL, "|", &save);  // charging

    if (p_tag && p_val && c_tag && c_val &&
        !strcmp(p_tag, "P") && !strcmp(c_tag, "C")) {
        ui_set_phone_batt(atoi(p_val), atoi(c_val) != 0);
        return true;
    }

    return false;
}

static void log_conn_security(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    int rc = ble_gap_conn_find(conn_handle, &desc);
    if (rc == 0) {
        ESP_LOGI(BLE_TAG,
                 "SEC: enc=%d auth=%d bond=%d keysz=%d",
                 desc.sec_state.encrypted,
                 desc.sec_state.authenticated,
                 desc.sec_state.bonded,
                 desc.sec_state.key_size);
    } else {
        ESP_LOGW(BLE_TAG, "SEC: ble_gap_conn_find failed rc=%d", rc);
    }
}

/* ---------------- GATT RX ---------------- */
static int gatt_chr_rx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    char chunk[256];
    int chunk_len = OS_MBUF_PKTLEN(ctxt->om);
    if (chunk_len <= 0) return 0;
    if (chunk_len >= (int)sizeof(chunk)) chunk_len = (int)sizeof(chunk) - 1;

    int rc = ble_hs_mbuf_to_flat(ctxt->om, chunk, chunk_len, NULL);
    if (rc != 0) return BLE_ATT_ERR_UNLIKELY;

    chunk[chunk_len] = '\0';

    // Overflow protection
    if (s_rx_len + (size_t)chunk_len >= sizeof(s_rx_accum)) {
        ESP_LOGW(BLE_TAG, "RX accum overflow; clearing buffer");
        s_rx_len = 0;
        s_rx_accum[0] = '\0';
    }

    memcpy(s_rx_accum + s_rx_len, chunk, (size_t)chunk_len);
    s_rx_len += (size_t)chunk_len;
    s_rx_accum[s_rx_len] = '\0';

    // Battery fast-path (if sent without newline)
    if (s_rx_len >= 2 && s_rx_accum[0] == 'B' && s_rx_accum[1] == '|') {
        int pipes = 0;
        for (size_t i = 0; i < s_rx_len; i++) if (s_rx_accum[i] == '|') pipes++;
        if (pipes >= 5) {
            if (parse_batt_frame(s_rx_accum)) {
                if (s_on_rx) s_on_rx(s_rx_accum, (int)strlen(s_rx_accum));
                s_rx_len = 0;
                s_rx_accum[0] = '\0';
            }
        }
    }

    // Process newline-delimited frames
    while (1) {
        char *nl = strchr(s_rx_accum, '\n');
        if (!nl) break;

        *nl = '\0';
        char *frame = s_rx_accum;

        size_t flen = strlen(frame);
        if (flen > 0 && frame[flen - 1] == '\r') frame[flen - 1] = '\0';

        if (frame[0] != '\0') {

                       // Battery frame
            if (frame[0] == 'B' && frame[1] == '|') {
                (void)parse_batt_frame(frame);
                if (s_on_rx) s_on_rx(frame, (int)strlen(frame));
            }
            // Notification frames
            else if (frame[0] == 'N' && frame[1] == '|') {

                size_t wlen = strlen(frame);
                if (wlen >= sizeof(s_work)) wlen = sizeof(s_work) - 1;
                memcpy(s_work, frame, wlen);
                s_work[wlen] = '\0';

                char *save = NULL;
                (void)strtok_r(s_work, "|", &save);        // N
                char *cmd = strtok_r(NULL, "|", &save);    // U/D/S/E or old TYPE

                if (cmd) {
                    if (!strcmp(cmd, "S")) {
                        notif_clear_all_local();
                        s_notif_snapshot_mode = true;
                        ui_notif_refresh_async();
                    }
                    else if (!strcmp(cmd, "E")) {
                        s_notif_snapshot_mode = false;
                        notif_recalc_counts();
                        ui_notif_refresh_async();
                    }
                    else if (!strcmp(cmd, "D")) {
                        char *id = strtok_r(NULL, "|", &save);
                        if (id && *id) {
                            int idx = notif_find(id);
                            if (idx >= 0) s_notifs[idx].used = false;

                            notif_recalc_counts();
                            if (!s_notif_snapshot_mode) ui_notif_refresh_async();
                        }
                    }
                    else if (!strcmp(cmd, "U")) {
                        char *id    = strtok_r(NULL, "|", &save);
                        char *type  = strtok_r(NULL, "|", &save);
                        char *pkg   = strtok_r(NULL, "|", &save);
                        char *title = strtok_r(NULL, "|", &save);
                        char *body  = strtok_r(NULL, "|", &save);
                        (void)title; (void)body;

                        if (id && *id && type && *type) {
                            notif_type_t g = notif_group_from_pkg_and_type(type, pkg);

                            int idx = notif_find(id);
                            if (idx < 0) idx = notif_free_slot();

                            if (idx >= 0) {
                                s_notifs[idx].used  = true;
                                s_notifs[idx].group = g;
                                strncpy(s_notifs[idx].id, id, NOTIF_ID_MAX - 1);
                                s_notifs[idx].id[NOTIF_ID_MAX - 1] = '\0';
                            }

                            notif_recalc_counts();
                            if (!s_notif_snapshot_mode) ui_notif_refresh_async();
                        }
                    }
                    else {
                        // Backward compat: N|TYPE|PKG|Title|Body
                        char *type = cmd;
                        char *pkg  = strtok_r(NULL, "|", &save);
                        (void)strtok_r(NULL, "|", &save);
                        (void)strtok_r(NULL, "|", &save);

                        notif_type_t g = notif_group_from_pkg_and_type(type, pkg);
                        ui_notif_add_from_ble(g);
                    }
                }

                if (s_on_rx) s_on_rx(frame, (int)strlen(frame));
            }
            // Text/message frames (NOW at correct level)
            else if (frame[0] == 'T' && frame[1] == '|') {

                size_t wlen = strlen(frame);
                if (wlen >= sizeof(s_work)) wlen = sizeof(s_work) - 1;
                memcpy(s_work, frame, wlen);
                s_work[wlen] = '\0';

                char *save = NULL;
                (void)strtok_r(s_work, "|", &save); // T
                char *cmd   = strtok_r(NULL, "|", &save); // U
                char *msgId = strtok_r(NULL, "|", &save);
                char *tsMs  = strtok_r(NULL, "|", &save);
                char *thread_b64 = strtok_r(NULL, "|", &save);
                char *sender_b64 = strtok_r(NULL, "|", &save);
                char *body_b64   = strtok_r(NULL, "|", &save);
                char *pkg_b64    = strtok_r(NULL, "|", &save);

                if (cmd && !strcmp(cmd, "U") &&
                    msgId && tsMs && thread_b64 && sender_b64 && body_b64 && pkg_b64) {

                    uint32_t mid = (uint32_t)strtoul(msgId, NULL, 10);
                    uint64_t tms = (uint64_t)strtoull(tsMs, NULL, 10);

                    static char thread_id[128];
                    static char sender[96];
                    static char body[1024];
                    static char pkg[96];
                    thread_id[0] = sender[0] = body[0] = pkg[0] = '\0';


                    if (b64url_decode_to(thread_id, sizeof(thread_id), thread_b64) == ESP_OK &&
                        b64url_decode_to(sender,    sizeof(sender),    sender_b64) == ESP_OK &&
                        b64url_decode_to(body,      sizeof(body),      body_b64)   == ESP_OK &&
                        b64url_decode_to(pkg,       sizeof(pkg),       pkg_b64)    == ESP_OK) {

                        (void)watch_sms_store_ingest(mid, tms, thread_id, sender, body, pkg);
                    }
                }

                if (s_on_rx) s_on_rx(frame, (int)strlen(frame));
            }
            else {
                if (s_on_rx) s_on_rx(frame, (int)strlen(frame));
            }

        }

        // consume processed frame
        size_t consumed = (size_t)(nl - s_rx_accum) + 1;
        memmove(s_rx_accum, s_rx_accum + consumed, s_rx_len - consumed);
        s_rx_len -= consumed;
        s_rx_accum[s_rx_len] = '\0';
    }

    // Optional ACK (queue-based; safe)
    ble_txq_send_str("OK");

    return 0;
}

/* ---------------- TX (read) ---------------- */
static int gatt_chr_tx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const char *s = "watch_tx";
        os_mbuf_append(ctxt->om, s, strlen(s));
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* ---------------- GATT definition ---------------- */
static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC_NOTIF.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid      = &UUID_CHR_RX.u,
                .access_cb = gatt_chr_rx_access_cb,
                .flags     = BLE_GATT_CHR_F_WRITE
                           | BLE_GATT_CHR_F_WRITE_NO_RSP
                           | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid       = &UUID_CHR_TX.u,
                .access_cb  = gatt_chr_tx_access_cb,
                .val_handle = &s_tx_val_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY
                           | BLE_GATT_CHR_F_READ
                           | BLE_GATT_CHR_F_READ_ENC,
            },
            { 0 }
        },
    },
    { 0 }
};

/* ---------------- GAP event handler ---------------- */
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {

        case BLE_GAP_EVENT_CONNECT:
            ESP_LOGI(BLE_TAG, "CONNECT status=%d", event->connect.status);

            if (event->connect.status == 0) {
                s_rx_len = 0;
                s_rx_accum[0] = '\0';

                s_conn_handle = event->connect.conn_handle;
                g_ble_connected = true;

                // Try to initiate encryption if not already encrypted
                struct ble_gap_conn_desc d;
                if (ble_gap_conn_find(s_conn_handle, &d) == 0) {
                    if (!d.sec_state.encrypted) {
                        int rc = ble_gap_security_initiate(s_conn_handle);
                        ESP_LOGI(BLE_TAG, "security_initiate rc=%d", rc);
                    }
                }

                // Clear notifications (battery persists until phone sends)
                notif_clear_all_local();
                s_notif_snapshot_mode = false;

                watch_audio_beep_async(1900, 80);
                ble_ui_mark_dirty();
            } else {
                s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                s_notify_enabled = false;
                g_ble_connected = false;

                ble_ui_mark_dirty();
                if (s_ble_enabled) ble_advertise();
            }

            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_notify_enabled = false;
        g_ble_connected = false;

        notif_clear_all_local();
        phone_batt_clear_local();
        s_notif_snapshot_mode = false;

        watch_audio_beep_async(1400, 60);

        s_rx_len = 0;
        s_rx_accum[0] = '\0';

        ble_ui_mark_dirty();
        if (s_ble_enabled) {
            ble_advertise();
        }
        ESP_LOGI(BLE_TAG, "State: Disconnect, Reason=%d",
                 event->disconnect.reason);
        if (s_txq) xQueueReset(s_txq);
        return 0;


        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == s_tx_val_handle) {
                s_notify_enabled = event->subscribe.cur_notify;
                ESP_LOGI(BLE_TAG, "TX notify %s", s_notify_enabled ? "ENABLED" : "DISABLED");
            }
            return 0;

        case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_ble_enabled) ble_advertise();
        ESP_LOGI(BLE_TAG, "State: ADV Complete");
        return 0;


        case BLE_GAP_EVENT_PASSKEY_ACTION:
            ESP_LOGI(BLE_TAG, "PASSKEY_ACTION (io=%d)", ble_hs_cfg.sm_io_cap);
            return 0;

        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGI(BLE_TAG, "ENC_CHANGE status=%d", event->enc_change.status);
            if (event->enc_change.status == 0) {
                log_conn_security(event->enc_change.conn_handle);
            }
            return 0;

        case BLE_GAP_EVENT_REPEAT_PAIRING:
            return BLE_GAP_REPEAT_PAIRING_RETRY;

        default:
            return 0;
    }
}

/* ---------------- Connection parameter profiles ---------------- */
void ble_request_sleep_params(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;

    struct ble_gap_upd_params p = {0};

    // interval units = 1.25ms
    // 640 = 800ms
    p.itvl_min = 640;   // 800ms
    p.itvl_max = 960;   // 1200ms

    // allow skipping many events
    p.latency  = 30;    // effective wake can be ~ (interval * (latency+1))

    // supervision_timeout units = 10ms
    p.supervision_timeout = 3000; // 30s

    int rc = ble_gap_update_params(s_conn_handle, &p);
    ESP_LOGI(BLE_TAG, "BLE params -> SLEEP rc=%d (800-1200ms, lat=30, to=30s)", rc);
}


void ble_request_awake_params(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;

    struct ble_gap_upd_params p = {0};
    p.itvl_min = 24;   // 30ms
    p.itvl_max = 40;   // 50ms
    p.latency  = 0;
    p.supervision_timeout = 6000; // 60s

    int rc = ble_gap_update_params(s_conn_handle, &p);
    ESP_LOGI(BLE_TAG, "BLE params -> AWAKE rc=%d", rc);
}

/* ---------------- Advertising ---------------- */
static void ble_stop_adv_only(void)
{
    (void)ble_gap_adv_stop();
}

static void ble_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields  adv_fields;
    struct ble_hs_adv_fields  rsp_fields;

    memset(&adv_params, 0, sizeof(adv_params));
    memset(&adv_fields, 0, sizeof(adv_fields));
    memset(&rsp_fields, 0, sizeof(rsp_fields));

    ble_stop_adv_only();

    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    const char *name = ble_svc_gap_device_name();
    adv_fields.name = (uint8_t *)name;
    adv_fields.name_len = strlen(name);
    adv_fields.name_is_complete = 1;

    adv_fields.tx_pwr_lvl_is_present = 1;
    adv_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    adv_fields.appearance_is_present = 1;
    adv_fields.appearance = 0x0341; // Generic Watch

    int rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "ble_gap_adv_set_fields rc=%d", rc);
        return;
    }

    rsp_fields.uuids128 = (const ble_uuid128_t *)&UUID_SVC_NOTIF;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "ble_gap_adv_rsp_set_fields rc=%d", rc);
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    // intervals units = 0.625ms
    // 1600 = 1000ms, 3200 = 2000ms
    adv_params.itvl_min = 1600;   // 1.0s
    adv_params.itvl_max = 3200;   // 2.0s


    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_cb, NULL);

    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "ble_gap_adv_start rc=%d (addr_type=%u)", rc, (unsigned)s_own_addr_type);
    } else {
        ble_ui_mark_dirty();
    }
}

/* ---------------- Host sync ---------------- */
static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(1, &s_own_addr_type);
    if (rc != 0) {
        rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
        if (rc != 0) {
            ESP_LOGW(BLE_TAG, "infer_auto failed rc=%d; using addr_type=%u",
                     rc, (unsigned)s_own_addr_type);
        }
    }

    // Only advertise if user intent says BLE is enabled
    if (s_ble_enabled) {
        ble_advertise();
    } else {
        ble_stop_adv_only();
    }
}


static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---------------- Public API ---------------- */
esp_err_t ble_init(ble_rx_cb_t on_rx)
{
    s_on_rx = on_rx;
    // Mirror user setting (Settings owns g_ble_on)
    extern bool g_ble_on;
    s_ble_enabled = g_ble_on;

    // NVS required for bonding storage
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(BLE_TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    nimble_port_init();
    ble_store_config_init();

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("NoesisWatch");

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) return ESP_FAIL;

    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) return ESP_FAIL;

    ble_hs_cfg.sync_cb = on_sync;

    // Security / bonding
    ble_hs_cfg.store_status_cb = ble_store_status_cb;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0; // Just Works
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;

    // TX queue/task
    if (!s_txq) {
        s_txq = xQueueCreate(BLE_TXQ_DEPTH, sizeof(ble_tx_item_t));
    }
    if (s_txq && !s_tx_task) {
        xTaskCreate(ble_txq_task, "ble_txq", 8192, NULL, 5, &s_tx_task);
    }

    nimble_port_freertos_init(host_task);

    ESP_LOGI(BLE_TAG, "BLE init done");
    return ESP_OK;
}

void ble_start(void)
{
    if (!s_ble_enabled) return;
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ble_advertise();
    }
}

void ble_stop(void)
{
    // Stop advertising
    ble_stop_adv_only();

    // Disconnect if connected
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }

    s_notify_enabled = false;
    g_ble_connected = false;

    // Optional but consistent: clear phone batt/notifications on explicit stop
    notif_clear_all_local();
    phone_batt_clear_local();
    s_notif_snapshot_mode = false;

    ble_ui_mark_dirty();
}


bool ble_is_connected(void)
{
    return (s_conn_handle != BLE_HS_CONN_HANDLE_NONE);
}

esp_err_t ble_notify_tx(const char *msg)
{
    if (!msg) return ESP_ERR_INVALID_ARG;
    if (!s_notify_enabled) return ESP_ERR_INVALID_STATE;
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_tx_val_handle == 0) return ESP_ERR_INVALID_STATE;
    if (!s_txq) return ESP_ERR_INVALID_STATE;

    ble_tx_item_t it = {0};
    it.conn = s_conn_handle;
    it.val_handle = s_tx_val_handle;

    size_t n = strlen(msg);
    if (n >= BLE_TX_MAX_BYTES) n = BLE_TX_MAX_BYTES - 1;
    memcpy(it.data, msg, n);
    it.data[n] = '\0';
    it.len = (uint16_t)n;

    if (xQueueSend(s_txq, &it, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM; // queue full -> drop
    }
    return ESP_OK;
}
void ble_set_enabled(bool on)
{
    s_ble_enabled = on;

    if (on) {
        // Respect init state: if NimBLE isn't ready yet, ble_start() will safely advertise when possible.
        ble_start();
    } else {
        // Hard stop: stop advertising and disconnect if connected.
        ble_stop();
    }

    ble_ui_mark_dirty();
}

bool ble_is_enabled(void)
{
    return s_ble_enabled;
}
