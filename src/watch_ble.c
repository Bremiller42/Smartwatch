// watch_ble.c (FULL, drop-in, minimally corrected)

#include "watch_ble.h"
#include "watch_globals.h"
#include "watch_ui.h"
#include "watch_audio.h"
#include <string.h>
#include <stdio.h>

#include <lvgl.h>

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

// NimBLE / host
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/ble.h"
#include "host/ble_hs.h"

#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *BLE_TAG = "BLE";

/* ---------------- Internal State ---------------- */
static ble_rx_cb_t s_on_rx = NULL;

static uint16_t s_conn_handle   = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_val_handle = 0;   // handle for TX characteristic value
static bool     s_notify_enabled = false;

// Address type used for advertising (infer at sync)
static uint8_t  s_own_addr_type = BLE_OWN_ADDR_PUBLIC;

/* ---------------- UUIDs (custom 128-bit) ----------------
   Keep these consistent with your Android app/client.
*/
static const ble_uuid128_t UUID_SVC_NOTIF = BLE_UUID128_INIT(
    0x2d,0x1a,0x9b,0x24,0x9c,0xb1,0x4e,0x9c,0x9c,0x1d,0x37,0x3a,0x7e,0x8f,0x4a,0x10);

static const ble_uuid128_t UUID_CHR_RX = BLE_UUID128_INIT(
    0x2d,0x1a,0x9b,0x24,0x9c,0xb1,0x4e,0x9c,0x9c,0x1d,0x37,0x3a,0x7e,0x8f,0x4a,0x11);

static const ble_uuid128_t UUID_CHR_TX = BLE_UUID128_INIT(
    0x2d,0x1a,0x9b,0x24,0x9c,0xb1,0x4e,0x9c,0x9c,0x1d,0x37,0x3a,0x7e,0x8f,0x4a,0x12);

static void ble_advertise(void);
static void ble_stop_adv_only(void);
static volatile bool s_ble_ui_dirty = false;

static char   s_rx_accum[1024];
static size_t s_rx_len = 0;

/* ---------------- Notification state table ---------------- */
#define NOTIF_MAX_ITEMS 30
#define NOTIF_ID_MAX    256

typedef struct {
    bool used;
    char id[NOTIF_ID_MAX];
    notif_type_t group;
} notif_item_t;

static notif_item_t s_notifs[NOTIF_MAX_ITEMS];

// Snapshot handling (your protocol N|S ... N|E)
static bool s_notif_snapshot_mode = false;
static bool s_notif_snapshot_dirty = false;

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

// Clear only battery UI/state (call on DISCONNECT if you want it blank when phone gone)
static void phone_batt_clear_local(void)
{
    g_phone_batt_pct = -1;
    g_phone_batt_charging = false;
    lv_async_call(ui_hide_phone_batt_cb, NULL);
}

// IMPORTANT FIX:
// Clearing notifications must NOT hide battery on connect.
static void notif_clear_all_local(void)
{
    memset(s_notifs, 0, sizeof(s_notifs));
    for (int i = 0; i < NG_MAX; i++) g_notif_counts[i] = 0;

    // DO NOT touch phone battery here.
    // Battery is its own thing, not part of notification snapshot.
}

static void notif_recalc_counts(void)
{
    for (int i = 0; i < NG_MAX; i++) g_notif_counts[i] = 0;

    for (int i = 0; i < NOTIF_MAX_ITEMS; i++) {
        if (!s_notifs[i].used) continue;
        notif_type_t g = s_notifs[i].group;
        if (g >= 0 && g < NG_MAX && g_notif_counts[g] < 999) {
            g_notif_counts[g]++;
        }
    }
}

/* ---------------- Small UI helpers ---------------- */
static void ble_ui_mark_dirty(void)
{
    s_ble_ui_dirty = true;
}

void ble_ui_mark_dirty_from_ble_thread(void)
{
    s_ble_ui_dirty = true;
}

bool ble_ui_take_dirty(void)
{
    if (!s_ble_ui_dirty) return false;
    s_ble_ui_dirty = false;
    return true;
}

static notif_type_t notif_group_from_pkg_and_type(const char *type, const char *pkg)
{
    // 1) Package-based rules first (most specific)
    if (pkg && *pkg) {
        if (strstr(pkg, "com.google.android.youtube")) return NG_YOUTUBE;
        if (strstr(pkg, "tv.twitch.android.app"))      return NG_APP;      // until you add Twitch icon
        if (strstr(pkg, "com.reddit.frontpage"))       return NG_REDDIT;
        if (strstr(pkg, "com.facebook.orca"))          return NG_MESSENGER;
        if (strstr(pkg, "discord"))                    return NG_DISCORD;
        if (strstr(pkg, "amazon"))                     return NG_AMAZON;

        // Textra: treat as SMS group
        if (strstr(pkg, "com.textra"))                 return NG_SMS;

        // Pocket: you can give it its own later; for now put it under APP
        if (strstr(pkg, "heypocket") || strstr(pkg, "getpocket")) return NG_APP;

        // Google app often delivers weather cards
        if (strstr(pkg, "googlequicksearchbox"))       return NG_WEATHER;
    }

    // 2) Type-based fallback (coarse)
    if (type) {
        if (!strcmp(type, "SMS"))     return NG_SMS;
        if (!strcmp(type, "EMAIL"))   return NG_EMAIL;
        if (!strcmp(type, "SYS"))     return NG_SYSTEM;
        if (!strcmp(type, "WEATHER")) return NG_WEATHER;
    }

    // 3) Unknown -> generic app bucket
    return NG_APP;
}

// Parse a battery frame: "B|P|<pct>|C|<0|1>"
static bool parse_batt_frame(const char *frame)
{
    if (!frame) return false;
    if (!(frame[0] == 'B' && frame[1] == '|')) return false;

    char work[64];
    strncpy(work, frame, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    char *save = NULL;
    (void)strtok_r(work, "|", &save);       // "B"
    char *p_tag = strtok_r(NULL, "|", &save); // "P"
    char *p_val = strtok_r(NULL, "|", &save); // pct
    char *c_tag = strtok_r(NULL, "|", &save); // "C"
    char *c_val = strtok_r(NULL, "|", &save); // charging

    if (p_tag && p_val && c_tag && c_val &&
        !strcmp(p_tag, "P") && !strcmp(c_tag, "C")) {
        ui_set_phone_batt(atoi(p_val), atoi(c_val) != 0);
        return true;
    }

    return false;
}

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
    ESP_LOGI(BLE_TAG, "RX chunk (%d): %s", chunk_len, chunk);

    /* If incoming data would overflow buffer, reset (fail-safe). */
    if (s_rx_len + (size_t)chunk_len >= sizeof(s_rx_accum)) {
        ESP_LOGW(BLE_TAG, "RX accum overflow; clearing buffer");
        s_rx_len = 0;
        s_rx_accum[0] = '\0';
    }

    /* Append chunk into accumulator (FIX: append BEFORE any fallback parsing) */
    memcpy(s_rx_accum + s_rx_len, chunk, (size_t)chunk_len);
    s_rx_len += (size_t)chunk_len;
    s_rx_accum[s_rx_len] = '\0';

    // -------- Battery fallback: allow B frames even if '\n' hasn't arrived yet --------
    // If buffer begins with B| and has enough separators, parse immediately and clear.
    if (s_rx_len >= 2 && s_rx_accum[0] == 'B' && s_rx_accum[1] == '|') {
        int pipes = 0;
        for (size_t i = 0; i < s_rx_len; i++) {
            if (s_rx_accum[i] == '|') pipes++;
        }
        if (pipes >= 5) {
            if (parse_batt_frame(s_rx_accum)) {
                s_rx_len = 0;
                s_rx_accum[0] = '\0';
            }
        }
    }

    /* Process complete lines (newline-delimited frames) */
    while (1) {
        char *nl = strchr(s_rx_accum, '\n');
        if (!nl) break;

        *nl = '\0';             // terminate this frame
        char *frame = s_rx_accum;

        // Trim optional '\r'
        size_t flen = strlen(frame);
        if (flen > 0 && frame[flen - 1] == '\r') frame[flen - 1] = '\0';

        if (frame[0] != '\0') {
            ESP_LOGI(BLE_TAG, "RX frame: %s", frame);

            // Battery frame
            if (frame[0] == 'B' && frame[1] == '|') {
                (void)parse_batt_frame(frame);

                // Optional pass-through callback
                if (s_on_rx) s_on_rx(frame, (int)strlen(frame));
            }
            // Notification frames
            else if (frame[0] == 'N' && frame[1] == '|') {

                char work[1024];
                size_t wlen = strlen(frame);
                if (wlen >= sizeof(work)) wlen = sizeof(work) - 1;
                memcpy(work, frame, wlen);
                work[wlen] = '\0';

                char *save = NULL;
                (void)strtok_r(work, "|", &save);      // "N"
                char *cmd = strtok_r(NULL, "|", &save); // "U"/"D"/"S"/"E" or old TYPE

                if (cmd) {
                    // Snapshot start: N|S
                    if (!strcmp(cmd, "S")) {
                        ESP_LOGI(BLE_TAG, "Notif snapshot START");

                        notif_clear_all_local();       // clear notif state ONLY
                        s_notif_snapshot_mode = true;
                        s_notif_snapshot_dirty = true; // will refresh once at end
                    }
                    // Snapshot end: N|E
                    else if (!strcmp(cmd, "E")) {
                        ESP_LOGI(BLE_TAG, "Notif snapshot END");

                        s_notif_snapshot_mode = false;

                        notif_recalc_counts();
                        ui_notif_refresh_async();
                    }
                    // Delete: N|D|id
                    else if (!strcmp(cmd, "D")) {
                        char *id = strtok_r(NULL, "|", &save);
                        if (id && *id) {
                            int idx = notif_find(id);
                            if (idx >= 0) {
                                s_notifs[idx].used = false;
                                ESP_LOGI(BLE_TAG, "Notif delete id=%s", id);
                            } else {
                                ESP_LOGI(BLE_TAG, "Notif delete id=%s (not found)", id);
                            }

                            notif_recalc_counts();
                            if (s_notif_snapshot_mode) {
                                s_notif_snapshot_dirty = true;
                            } else {
                                ui_notif_refresh_async();
                            }
                        }
                    }
                    // Upsert: N|U|id|type|pkg|title|body
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

                                ESP_LOGI(BLE_TAG, "Notif upsert id=%s group=%d",
                                         s_notifs[idx].id, (int)g);
                            } else {
                                ESP_LOGW(BLE_TAG, "Notif table full; dropping id=%s", id);
                            }

                            notif_recalc_counts();
                            if (s_notif_snapshot_mode) {
                                s_notif_snapshot_dirty = true;
                            } else {
                                ui_notif_refresh_async();
                            }
                        }
                    }
                    // Backward compat: old format N|TYPE|PKG|Title|Body
                    else {
                        char *type  = cmd;
                        char *pkg   = strtok_r(NULL, "|", &save);
                        char *title = strtok_r(NULL, "|", &save);
                        char *body  = strtok_r(NULL, "|", &save);
                        (void)title; (void)body;

                        notif_type_t g = notif_group_from_pkg_and_type(type, pkg);
                        ui_notif_add_from_ble(g);
                    }
                }

                // Optional pass-through callback
                if (s_on_rx) s_on_rx(frame, (int)strlen(frame));
            }
            // Anything else
            else {
                if (s_on_rx) s_on_rx(frame, (int)strlen(frame));
            }
        }

        /* Remove processed frame from accumulator */
        size_t consumed = (size_t)(nl - s_rx_accum) + 1; // +1 for '\n'
        memmove(s_rx_accum, s_rx_accum + consumed, s_rx_len - consumed);
        s_rx_len -= consumed;
        s_rx_accum[s_rx_len] = '\0';
    }

    /* Optional ACK back via notify on TX (only if phone subscribed) */
    if (s_notify_enabled && s_conn_handle != BLE_HS_CONN_HANDLE_NONE && s_tx_val_handle) {
        const char *ack = "OK";
        struct os_mbuf *om = ble_hs_mbuf_from_flat(ack, strlen(ack));
        if (om) {
            (void)ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
        }
    }

    return 0;
}

/* ---------------- TX characteristic access (read) ---------------- */
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
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid       = &UUID_CHR_TX.u,
                .access_cb  = gatt_chr_tx_access_cb,
                .val_handle = &s_tx_val_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ,
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
            if (event->connect.status == 0) {

                // Reset RX accumulator for a clean session
                s_rx_len = 0;
                s_rx_accum[0] = '\0';

                s_conn_handle = event->connect.conn_handle;
                g_ble_connected = true;

                // IMPORTANT FIX:
                // Do NOT hide/clear battery on connect.
                // Only clear notification state.
                notif_clear_all_local();
                s_notif_snapshot_mode = false;
                s_notif_snapshot_dirty = false;

                ESP_LOGI(BLE_TAG, "Connected (handle=%d)", s_conn_handle);
                watch_audio_beep_async(1900, 100);

                ble_ui_mark_dirty();

            } else {
                ESP_LOGW(BLE_TAG, "Connect failed (status=%d); restarting adv", event->connect.status);
                s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                s_notify_enabled = false;
                g_ble_connected = false;

                ble_ui_mark_dirty();
                ble_advertise();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(BLE_TAG, "Disconnected; reason=%d", event->disconnect.reason);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_notify_enabled = false;
            g_ble_connected = false;

            // Clear notif state + battery when phone is gone
            notif_clear_all_local();
            phone_batt_clear_local();
            s_notif_snapshot_mode = false;
            s_notif_snapshot_dirty = false;

            watch_audio_beep_async(1400, 80);

            // Reset RX accumulator for a clean session
            s_rx_len = 0;
            s_rx_accum[0] = '\0';

            ble_ui_mark_dirty();
            ble_advertise();
            return 0;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == s_tx_val_handle) {
                s_notify_enabled = event->subscribe.cur_notify;
                ESP_LOGI(BLE_TAG, "TX notify %s",
                         s_notify_enabled ? "ENABLED" : "DISABLED");
            }
            return 0;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            ESP_LOGI(BLE_TAG, "Adv complete; restarting");
            ble_advertise();
            return 0;

        default:
            return 0;
    }
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

    adv_params.itvl_min = 0x0030; // 30ms
    adv_params.itvl_max = 0x0060; // 60ms

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "ble_gap_adv_start rc=%d (addr_type=%u)",
                 rc, (unsigned)s_own_addr_type);
    } else {
        ESP_LOGI(BLE_TAG, "Advertising started (addr_type=%u)", (unsigned)s_own_addr_type);
        ble_ui_mark_dirty();
    }
}

/* ---------------- Host sync ---------------- */
static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(1, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGW(BLE_TAG, "ble_hs_id_infer_auto(1) rc=%d; fallback infer_auto(0)", rc);
        rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
        if (rc != 0) {
            ESP_LOGW(BLE_TAG, "infer_auto(0) rc=%d; using default addr_type=%u",
                     rc, (unsigned)s_own_addr_type);
        }
    }

    ble_advertise();
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

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else if (nvs != ESP_OK && nvs != ESP_ERR_NVS_NOT_INITIALIZED) {
        ESP_LOGW(BLE_TAG, "nvs init err=%d (might be ok if you init elsewhere)", (int)nvs);
    }

    nimble_port_init();

    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_svc_gap_device_name_set("NoesisWatch");

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "ble_gatts_count_cfg rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "ble_gatts_add_svcs rc=%d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(BLE_TAG, "Custom service registered, TX handle=%u", (unsigned)s_tx_val_handle);
    ESP_LOGI(BLE_TAG, "GATT services added. Device name=%s", ble_svc_gap_device_name());

    ble_hs_cfg.sync_cb = on_sync;

    nimble_port_freertos_init(host_task);

    ESP_LOGI(BLE_TAG, "BLE init done");
    return ESP_OK;
}

void ble_start(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ble_advertise();
    }
}

void ble_stop(void)
{
    ble_stop_adv_only();

    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }

    s_notify_enabled = false;
    g_ble_connected = false;

    ESP_LOGI(BLE_TAG, "BLE stopped");
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

    struct os_mbuf *om = ble_hs_mbuf_from_flat(msg, strlen(msg));
    if (!om) return ESP_ERR_NO_MEM;

    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}
