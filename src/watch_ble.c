#include "watch_ble.h"
#include "watch_globals.h"
#include "watch_ui.h"

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

static ble_rx_cb_t s_on_rx = NULL;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_val_handle = 0;     // handle for TX characteristic value
static bool s_notify_enabled = false;

/* ---- UUIDs (random custom 128-bit) ----
   You can regenerate later; just keep them consistent with your phone app.
*/
static const ble_uuid128_t UUID_SVC_NOTIF = BLE_UUID128_INIT(
    0x2d,0x1a,0x9b,0x24,0x9c,0xb1,0x4e,0x9c,0x9c,0x1d,0x37,0x3a,0x7e,0x8f,0x4a,0x10);

static const ble_uuid128_t UUID_CHR_RX = BLE_UUID128_INIT(
    0x2d,0x1a,0x9b,0x24,0x9c,0xb1,0x4e,0x9c,0x9c,0x1d,0x37,0x3a,0x7e,0x8f,0x4a,0x11);

static const ble_uuid128_t UUID_CHR_TX = BLE_UUID128_INIT(
    0x2d,0x1a,0x9b,0x24,0x9c,0xb1,0x4e,0x9c,0x9c,0x1d,0x37,0x3a,0x7e,0x8f,0x4a,0x12);

static void ble_advertise(void);

/* ---- RX write handler ---- */
static int gatt_chr_rx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    // Copy incoming bytes safely into a buffer and null-terminate
    char buf[256];
    int len = OS_MBUF_PKTLEN(ctxt->om);
    if (len <= 0) return 0;
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;

    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
    if (rc != 0) return BLE_ATT_ERR_UNLIKELY;

    buf[len] = '\0';

    ESP_LOGI(BLE_TAG, "RX write: %s", buf);

    if (s_on_rx) {
        s_on_rx(buf, len);
    }

    // Optional: reply back via notify on TX
    // (Only if phone subscribed)
    if (s_notify_enabled && s_conn_handle != BLE_HS_CONN_HANDLE_NONE && s_tx_val_handle) {
        // Tiny ACK to prove round-trip
        const char *ack = "OK";
        struct os_mbuf *om = ble_hs_mbuf_from_flat(ack, strlen(ack));
        if (om) {
            ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
        }
    }

    return 0;
}

/* ---- TX characteristic access (usually read, optional) ---- */
static int gatt_chr_tx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    // If you want TX to be readable too, you can return something here.
    // For now we just allow read with a static string.
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const char *s = "watch_tx";
        os_mbuf_append(ctxt->om, s, strlen(s));
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* ---- GATT definition ---- */
static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC_NOTIF.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &UUID_CHR_RX.u,
                .access_cb = gatt_chr_rx_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &UUID_CHR_TX.u,
                .access_cb = gatt_chr_tx_access_cb,
                .val_handle = &s_tx_val_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ,
            },
            { 0 } // terminator
        },
    },
    { 0 } // terminator
};

/* ---- GAP event handler ---- */
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_conn_handle = event->connect.conn_handle;
                g_ble_connected = true;
                lv_async_call(ui_update_ble_icon_async, NULL);
                ESP_LOGI(BLE_TAG, "Connected (handle=%d)", s_conn_handle);
            } else {
                ESP_LOGW(BLE_TAG, "Connect failed; restarting adv");
                s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                s_notify_enabled = false;
                ble_advertise();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(BLE_TAG, "Disconnected; reason=%d", event->disconnect.reason);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_notify_enabled = false;
            g_ble_connected = false;
            lv_async_call(ui_update_ble_icon_async, NULL);
            ble_advertise();
            return 0;

        case BLE_GAP_EVENT_SUBSCRIBE:
            // Track notify subscribe to TX characteristic
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

/* ---- Advertising ---- */
static void ble_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;

    memset(&fields, 0, sizeof(fields));
    memset(&adv_params, 0, sizeof(adv_params));

    // Device name shown to phone
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    // Advertise our service UUID
    fields.uuids128 = (ble_uuid128_t*)&UUID_SVC_NOTIF;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "ble_gap_adv_set_fields rc=%d", rc);
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "ble_gap_adv_start rc=%d", rc);
    } else {
        ESP_LOGI(BLE_TAG, "Advertising started");
    }
}

/* ---- NimBLE host sync ---- */
static void on_sync(void)
{
    // If you want to use a random static addr:
    // ble_hs_id_infer_auto(0, &addr_type);

    ble_advertise();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();         // runs host; returns on nimble_port_stop()
    nimble_port_freertos_deinit();
}

/* ---- Public API ---- */
esp_err_t ble_init(ble_rx_cb_t on_rx)
{
    s_on_rx = on_rx;

    // NimBLE uses NVS sometimes (bonds/keys), ensure NVS is ready.
    // If you already init NVS in main, this is harmless if called once there.
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else if (nvs != ESP_OK && nvs != ESP_ERR_NVS_NOT_INITIALIZED) {
        // If your app initializes NVS elsewhere, you may ignore NOT_INITIALIZED here.
        ESP_LOGW(BLE_TAG, "nvs init err=%d (might be ok if you init elsewhere)", (int)nvs);
    }

    nimble_port_init();

    ble_svc_gap_init();
    ble_svc_gatt_init();

    // Name shows up on phone scan list
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
    ble_gap_adv_stop();

    // Disconnect if connected
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    s_notify_enabled = false;
    ESP_LOGI(BLE_TAG, "BLE stopped");
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
