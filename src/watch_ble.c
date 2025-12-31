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
static notif_type_t notif_type_from_str(const char *s)
{
    if (!s) return NOTIF_APP;
    if (strcmp(s, "SMS") == 0)   return NOTIF_SMS;
    if (strcmp(s, "EMAIL") == 0) return NOTIF_EMAIL;
    if (strcmp(s, "MSG") == 0)   return NOTIF_MSG;
    if (strcmp(s, "TEST") == 0)  return NOTIF_APP; // your test type
    return NOTIF_APP;
}


/* ---------------- RX write handler ---------------- */
static int gatt_chr_rx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    char buf[256];
    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len <= 0) return 0;
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;

    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
    if (rc != 0) return BLE_ATT_ERR_UNLIKELY;

    buf[len] = '\0';
    ESP_LOGI(BLE_TAG, "RX write (%d): %s", len, buf);
    


    // buf contains message, e.g. "N|SMS|Title|Body"
    if (len >= 2 && buf[0] == 'N' && buf[1] == '|') {

        // Make a working copy we can tokenize (buf is already mutable)
        char *save = NULL;

        char *tok = strtok_r(buf, "|", &save); // "N"
        char *type = strtok_r(NULL, "|", &save); // "SMS"/"EMAIL"/"MSG"/etc
        // title/body are optional for just icons
        // char *title = strtok_r(NULL, "|", &save);
        // char *body  = strtok_r(NULL, "",  &save);

        notif_type_t t = notif_type_from_str(type);

        // ✅ safe from BLE thread:
        ui_notif_add_from_ble(t);
    }

    if (s_on_rx) {
        s_on_rx(buf, len);
    }
    // Optional ACK back via notify on TX (only if phone subscribed)
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
                .uuid      = &UUID_CHR_TX.u,
                .access_cb = gatt_chr_tx_access_cb,
                .val_handle = &s_tx_val_handle,
                .flags     = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ,
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
                s_conn_handle = event->connect.conn_handle;
                g_ble_connected = true;
                ESP_LOGI(BLE_TAG, "Connected (handle=%d)", s_conn_handle);
                watch_audio_beep_async(1900, 100);

                // Once connected, you are no longer advertising
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
            watch_audio_beep_async(1400, 80);

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
    // Safe to call even if not advertising
    int rc = ble_gap_adv_stop();
    if (rc != 0) {
        // NimBLE prints its own "stop advertising" logs; keep ours quiet unless debugging
    }
}

static void ble_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields  adv_fields;
    struct ble_hs_adv_fields  rsp_fields;

    memset(&adv_params, 0, sizeof(adv_params));
    memset(&adv_fields, 0, sizeof(adv_fields));
    memset(&rsp_fields, 0, sizeof(rsp_fields));

    // Stop current advertising before re-start
    ble_stop_adv_only();

    /* --- ADV packet (<= 31 bytes) ---
       Keep it small so Android sees it reliably.
       Put NAME here.
    */
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    const char *name = ble_svc_gap_device_name();
    adv_fields.name = (uint8_t *)name;
    adv_fields.name_len = strlen(name);
    adv_fields.name_is_complete = 1;

    // Optional: include TX power (helps some scanners show data)
    adv_fields.tx_pwr_lvl_is_present = 1;
    adv_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    int rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "ble_gap_adv_set_fields rc=%d", rc);
        return;
    }

    /* --- Scan Response packet (extra 31 bytes) ---
       Put 128-bit UUID here so we don't blow the ADV size.
    */
    rsp_fields.uuids128 = (const ble_uuid128_t *)&UUID_SVC_NOTIF;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;


    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "ble_gap_adv_rsp_set_fields rc=%d", rc);
        return;
    }

    // Connectable + discoverable
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    // Ensure SCANNABLE so scan response is actually used.
    // NOTE: Some NimBLE versions may not have this field.
    // If you get a compile error: remove this line and tell me the error text.
#ifdef __GNUC__
    // Try to set scannable if present in struct
    // (No portable reflection in C; this is here so you remember why.)
#endif
    // Make it "snappy" for Android scan lists
    adv_params.itvl_min = 0x0030; // 30ms
    adv_params.itvl_max = 0x0060; // 60ms

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "ble_gap_adv_start rc=%d (addr_type=%u)",
                 rc, (unsigned)s_own_addr_type);
    } else {
        ESP_LOGI(BLE_TAG, "Advertising started (addr_type=%u)", (unsigned)s_own_addr_type);
        // Not connected, but BLE is ON -> should show "Advertising"
        ble_ui_mark_dirty();

    }
}

/* ---------------- Host sync ---------------- */
static void on_sync(void)
{
    // Prefer random static address for ESP32 reliability
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
    nimble_port_run();             // runs host; returns on nimble_port_stop()
    nimble_port_freertos_deinit();
}

/* ---------------- Public API ---------------- */
esp_err_t ble_init(ble_rx_cb_t on_rx)
{
    s_on_rx = on_rx;
    // Ensure NVS is ready (NimBLE may use it for keys/bonds)
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

    // Name shown to Android scanners
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

    ESP_LOGI(BLE_TAG, "Custom service registered, TX handle=%u",
         (unsigned)s_tx_val_handle);
    ESP_LOGI(BLE_TAG, "GATT services added. Device name=%s", ble_svc_gap_device_name());

    // Host sync callback starts advertising
    ble_hs_cfg.sync_cb = on_sync;

    nimble_port_freertos_init(host_task);

    ESP_LOGI(BLE_TAG, "BLE init done");
    return ESP_OK;
}

void ble_start(void)
{
    // If not connected, ensure advertising is on
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
