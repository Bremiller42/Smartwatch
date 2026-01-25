// FILE: watch_wifi.c

#include "watch_wifi.h"
#include "watch_globals.h"
#include "ui_priv.h"
#include "watch_time.h"
#include "watch_settings.h"
#include "watch_audio.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <stdlib.h>
#include <string.h>

/* ---------------- WIFI_TAG ---------------- */
const char *WIFI_TAG = "WIFI";

/* ---------------- Event bits ----------------
   These must match whatever your watch_wifi.h expects.
   If they already exist in the header, remove these defines here. */
#ifndef WIFI_CONNECTED_BIT
#define WIFI_CONNECTED_BIT BIT0
#endif
#ifndef WIFI_FAIL_BIT
#define WIFI_FAIL_BIT      BIT1
#endif

/* ---------------- Internal helpers ---------------- */

static const char *auth_to_str(wifi_auth_mode_t a)
{
    switch (a) {
        case WIFI_AUTH_OPEN:          return "Open";
        case WIFI_AUTH_WEP:           return "WEP";
        case WIFI_AUTH_WPA_PSK:       return "WPA";
        case WIFI_AUTH_WPA2_PSK:      return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK:      return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        default:                      return "Sec";
    }
}

static inline void ui_wifi_icon_dirty_async(void)
{
    lv_async_call(ui_update_wifi_icon_async, NULL);
}

static inline void ui_ip_dirty_async(void)
{
    lv_async_call(ui_update_ip_label_async, NULL);
}

static bool wifi_stack_is_ready(void)
{
    return s_wifi_inited;
}

static void wifi_scan_done_to_ui(void *arg);
static void wifi_add_ap_to_list(const char *ssid, int rssi, wifi_auth_mode_t auth);

/* ---------------- Event handler ---------------- */

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(WIFI_TAG, "STA_START");

        // Auto-connect only if WiFi enabled AND we have saved SSID
        if (g_wifi_on && g_wifi_ssid[0] != '\0') {
            ESP_LOGI(WIFI_TAG, "Auto-connect: %s", g_wifi_ssid);
            (void)esp_wifi_connect();
        } else {
            ESP_LOGI(WIFI_TAG, "STA started (scan-only / wifi off / no creds)");
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_connected = false;
        ui_wifi_icon_dirty_async();

        // If user/sleep has WiFi OFF, do not retry.
        if (!g_wifi_on) {
            ESP_LOGI(WIFI_TAG, "DISCONNECTED (wifi off) -> no retry");
            return;
        }

        if (s_retry_num < WIFI_MAX_RETRY) {
            s_retry_num++;
            ESP_LOGW(WIFI_TAG, "Disconnected, retry %d/%d", s_retry_num, WIFI_MAX_RETRY);

            watch_audio_beep_async(990, 80);
            watch_audio_beep_async(660, 80);

            (void)esp_wifi_connect();
        } else {
            ESP_LOGE(WIFI_TAG, "Failed to connect (max retries)");
            if (s_wifi_evgrp) xEventGroupSetBits(s_wifi_evgrp, WIFI_FAIL_BIT);

            watch_audio_beep_async(990, 80);
            watch_audio_beep_async(660, 80);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        s_retry_num = 0;
        g_wifi_connected = true;
        ui_wifi_icon_dirty_async();

        watch_audio_beep_async(660, 80);
        watch_audio_beep_async(990, 80);

        ESP_LOGI(WIFI_TAG, "Connected: %s", g_wifi_ssid);
        ESP_LOGI(WIFI_TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        if (s_wifi_evgrp) xEventGroupSetBits(s_wifi_evgrp, WIFI_CONNECTED_BIT);

        snprintf(g_ip_str, sizeof(g_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ui_ip_dirty_async();

        // Time sync
        sntp_start();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        g_scan_in_progress = false;
        lv_async_call(wifi_scan_done_to_ui, NULL);
        return;
    }
}

/* ---------------- Public API ---------------- */

void wifi_ensure_started(void)
{
    // Event group for connect waits / status
    if (!s_wifi_evgrp) {
        s_wifi_evgrp = xEventGroupCreate();
        if (!s_wifi_evgrp) {
            ESP_LOGE(WIFI_TAG, "xEventGroupCreate failed");
            return;
        }
    }

    // netif init is safe to call once; track via global flags to avoid spam.
    if (!s_netif_inited) {
        esp_err_t e = esp_netif_init();
        if (e == ESP_OK) {
            s_netif_inited = true;
        } else if (e == ESP_ERR_INVALID_STATE) {
            s_netif_inited = true;
        } else {
            ESP_LOGE(WIFI_TAG, "esp_netif_init: %s", esp_err_to_name(e));
            return;
        }
    }

    if (!s_event_loop_inited) {
        esp_err_t e = esp_event_loop_create_default();
        if (e == ESP_OK || e == ESP_ERR_INVALID_STATE) {
            s_event_loop_inited = true;
        } else {
            ESP_LOGE(WIFI_TAG, "event loop create: %s", esp_err_to_name(e));
            return;
        }
    }

    // Create default STA netif once
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_sta_netif) {
            ESP_LOGE(WIFI_TAG, "esp_netif_create_default_wifi_sta failed");
            return;
        }
    }

    // Initialize WiFi driver once
    if (!s_wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t e = esp_wifi_init(&cfg);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(WIFI_TAG, "esp_wifi_init: %s", esp_err_to_name(e));
            return;
        }
        s_wifi_inited = true;
    }

    // Register event handlers once
    if (!s_handlers_registered) {
        esp_err_t e1 = esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &s_wifi_any_id_inst);

        esp_err_t e2 = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &s_got_ip_inst);

        if ((e1 != ESP_OK && e1 != ESP_ERR_INVALID_STATE) ||
            (e2 != ESP_OK && e2 != ESP_ERR_INVALID_STATE)) {
            ESP_LOGE(WIFI_TAG, "handler register failed: %s / %s",
                     esp_err_to_name(e1), esp_err_to_name(e2));
            // Don’t hard-fail; but mark not registered so we can retry later.
            s_handlers_registered = false;
        } else {
            s_handlers_registered = true;
        }
    }

    // Mode set is safe; ignore invalid state
    (void)esp_wifi_set_mode(WIFI_MODE_STA);

    // Start driver (OK if already started)
    esp_err_t st = esp_wifi_start();
    if (st != ESP_OK && st != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(WIFI_TAG, "esp_wifi_start: %s", esp_err_to_name(st));
        return;
    }
}

esp_err_t wifi_start_sta(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    wifi_ensure_started();
    if (!wifi_stack_is_ready()) return ESP_FAIL;

    // Respect WiFi toggle
    if (!g_wifi_on) {
        ESP_LOGW(WIFI_TAG, "wifi_start_sta ignored (wifi toggle off)");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_wifi_evgrp) xEventGroupClearBits(s_wifi_evgrp, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;

    // Disconnect current connection if any (safe if not connected)
    esp_err_t d = esp_wifi_disconnect();
    if (d != ESP_OK && d != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(WIFI_TAG, "esp_wifi_disconnect: %s", esp_err_to_name(d));
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", ssid);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", pass ? pass : "");

    cfg.sta.pmf_cfg.capable  = true;
    cfg.sta.pmf_cfg.required = false;

    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(WIFI_TAG, "esp_wifi_set_config: %s", esp_err_to_name(e));
        return e;
    }

    e = esp_wifi_connect();
    if (e != ESP_OK) {
        ESP_LOGE(WIFI_TAG, "esp_wifi_connect: %s", esp_err_to_name(e));
        return e;
    }

    ESP_LOGI(WIFI_TAG, "Connect requested (ssid=%s)", ssid);
    return ESP_OK;
}

void wifi_stop(void)
{
    // Make this safe if sleep calls it while WiFi isn't started yet
    g_wifi_connected = false;
    s_retry_num = 0;
    ui_wifi_icon_dirty_async();

    if (!wifi_stack_is_ready()) {
        ESP_LOGI(WIFI_TAG, "wifi_stop: stack not ready (noop)");
        // Still stop SNTP if it was running
        if (esp_sntp_enabled()) {
            esp_sntp_stop();
            ESP_LOGI(WIFI_TAG, "SNTP stopped");
        }
        g_time_synced = false;
        return;
    }

    // Stop scan if running (avoid weird scan state)
    if (g_scan_in_progress) {
        (void)esp_wifi_scan_stop();
        g_scan_in_progress = false;
    }

    (void)esp_wifi_disconnect();
    (void)esp_wifi_stop();

    ESP_LOGI(WIFI_TAG, "WiFi stopped");

    if (esp_sntp_enabled()) {
        esp_sntp_stop();
        ESP_LOGI(WIFI_TAG, "SNTP stopped");
    }

    g_time_synced = false;
}

void wifi_forget_saved(void)
{
    // Clear runtime
    g_wifi_ssid[0] = '\0';
    g_wifi_pass[0] = '\0';
    g_wifi_connected = false;
    s_retry_num = 0;

    // Stop stack
    wifi_stop();

    // Clear NVS
    settings_save_wifi_creds("", "");

    ESP_LOGW(WIFI_TAG, "Cleared saved WiFi credentials");
}

int wifi_get_rssi_dbm(int *out_rssi)
{
    if (!g_wifi_connected) return -1;
    if (!wifi_stack_is_ready()) return -1;

    wifi_ap_record_t ap;
    memset(&ap, 0, sizeof(ap));

    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err != ESP_OK) return -1;

    if (out_rssi) *out_rssi = ap.rssi;
    return 0;
}

/* ---------------- scanning ---------------- */

void start_wifi_scan(void)
{
    // Allow scanning even if user toggle is OFF (picker recovery)
    wifi_ensure_started();
    if (!wifi_stack_is_ready()) {
        ESP_LOGE(WIFI_TAG, "Cannot scan: wifi stack not ready");
        return;
    }

    if (g_scan_in_progress) {
        ESP_LOGW(WIFI_TAG, "Scan already in progress");
        return;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = 0,
        .bssid = 0,
        .channel = 0,
        .show_hidden = true
    };

    g_scan_in_progress = true;

    if (wifi_status_lbl) lv_label_set_text(wifi_status_lbl, "Scanning...");
    ESP_LOGI(WIFI_TAG, "Starting WiFi scan...");

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err == ESP_OK) return;

    g_scan_in_progress = false;

    ESP_LOGE(WIFI_TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
    if (wifi_status_lbl) {
        char st[64];
        snprintf(st, sizeof(st), "Scan failed: %s", esp_err_to_name(err));
        lv_label_set_text(wifi_status_lbl, st);
    }
}

static void wifi_add_ap_to_list(const char *ssid, int rssi, wifi_auth_mode_t auth)
{
    if (!wifi_list || !ssid || !ssid[0]) return;

    lv_obj_t *btn = lv_btn_create(wifi_list);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_add_event_cb(btn, (lv_event_cb_t)on_wifi_ap_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);

    char line[96];
    snprintf(line, sizeof(line), "%s  (%ddBm, %s)", ssid, rssi, auth_to_str(auth));
    lv_label_set_text(lbl, line);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, lv_pct(100));
}

static void wifi_scan_done_to_ui(void *arg)
{
    (void)arg;

    // If picker isn't open, ignore results
    if (!wifi_modal || !wifi_list) {
        ESP_LOGI(WIFI_TAG, "Scan done; picker not open -> ignore");
        return;
    }

    uint16_t ap_count = 0;
    esp_err_t e = esp_wifi_scan_get_ap_num(&ap_count);
    if (e != ESP_OK) {
        if (wifi_status_lbl) lv_label_set_text(wifi_status_lbl, "Scan read failed");
        return;
    }

    lv_obj_clean(wifi_list);

    if (wifi_status_lbl) {
        char st[64];
        snprintf(st, sizeof(st), "Found %u networks", (unsigned)ap_count);
        lv_label_set_text(wifi_status_lbl, st);
    }

    if (ap_count == 0) return;

    wifi_ap_record_t *aprs = (wifi_ap_record_t *)calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!aprs) return;

    uint16_t n = ap_count;
    if (esp_wifi_scan_get_ap_records(&n, aprs) == ESP_OK) {
        for (uint16_t i = 0; i < n; i++) {
            wifi_add_ap_to_list((const char *)aprs[i].ssid, aprs[i].rssi, aprs[i].authmode);
        }
    }

    free(aprs);
}
void watch_wifi_set_enabled(bool en)
{
    if (en) {
        // Respect user toggle
        if (!g_wifi_on) {
            ESP_LOGI(WIFI_TAG, "watch_wifi_set_enabled(true) ignored (wifi toggle off)");
            return;
        }

        wifi_ensure_started();

        // If we have creds, request connect. Safe if already connected.
        if (g_wifi_ssid[0] != '\0') {
            (void)esp_wifi_connect();
        }
    } else {
        wifi_stop();
    }
}
