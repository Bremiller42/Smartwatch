#include "watch_wifi.h"
#include "watch_globals.h"
#include "watch_ui.h"
#include "watch_time.h"
#include "watch_settings.h"
#include "watch_audio.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_err.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>


/* ---------------- WIFI_TAG ---------------- */
const char *WIFI_TAG = "WIFI";

/* ---------- helpers ---------- */
static const char* auth_to_str(wifi_auth_mode_t a)
{
    switch (a) {
        case WIFI_AUTH_OPEN: return "Open";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        default: return "Sec";
    }
}
void start_wifi_scan(void)
{
    // DON'T block scanning just because the "WiFi toggle" is off.
    // The picker should be able to scan so the user can recover.
    wifi_ensure_started();

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

    if (err == ESP_OK) {
        return;
    }

    // IMPORTANT: don't crash the whole app
    g_scan_in_progress = false;

    ESP_LOGE(WIFI_TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));

    if (wifi_status_lbl) {
        char st[64];
        snprintf(st, sizeof(st), "Scan failed: %s", esp_err_to_name(err));
        lv_label_set_text(wifi_status_lbl, st);
    }

    // If WiFi isn't started yet, you can optionally retry once:
    // if (err == ESP_ERR_WIFI_NOT_STARTED) { esp_wifi_start(); ... }
}

static void wifi_add_ap_to_list(const char *ssid, int rssi, wifi_auth_mode_t auth);
static void wifi_scan_done_to_ui(void *arg);

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(WIFI_TAG, "WiFi STA start");

        // Only connect if user has WiFi enabled AND we have creds
        if (g_wifi_on && g_wifi_ssid[0] != '\0') {
            ESP_LOGI(WIFI_TAG, "Auto-connect to saved SSID: %s", g_wifi_ssid);
            esp_wifi_connect();
        } else {
            ESP_LOGI(WIFI_TAG, "STA started (scan-only / no creds / wifi off)");
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    g_wifi_connected = false;
    lv_async_call(ui_update_wifi_icon_async, NULL);

    // If WiFi is toggled OFF, do NOT retry
    if (!g_wifi_on) {
        ESP_LOGI(WIFI_TAG, "Disconnected but WiFi is OFF -> no retry");
        return;
    }

    if (s_retry_num < WIFI_MAX_RETRY) {
        s_retry_num++;
        ESP_LOGW(WIFI_TAG, "WiFi disconnected, retry %d/%d", s_retry_num, WIFI_MAX_RETRY);
        watch_audio_beep_async(990, 80);
        watch_audio_beep_async(660, 80);
        esp_wifi_connect();
    } else {
        ESP_LOGE(WIFI_TAG, "WiFi failed to connect");
        xEventGroupSetBits(s_wifi_evgrp, WIFI_FAIL_BIT);
        watch_audio_beep_async(990, 80);
        watch_audio_beep_async(660, 80);
    }
}
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_retry_num = 0;
        g_wifi_connected = true;
        lv_async_call(ui_update_wifi_icon_async, NULL);

        watch_audio_beep_async(660, 80);
        watch_audio_beep_async(990, 80);

        ESP_LOGI(WIFI_TAG, "Connected to %s", g_wifi_ssid);
        ESP_LOGI(WIFI_TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_wifi_evgrp) xEventGroupSetBits(s_wifi_evgrp, WIFI_CONNECTED_BIT);

        snprintf(g_ip_str, sizeof(g_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        lv_async_call(ui_update_ip_label_async, NULL);

        sntp_start();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        g_scan_in_progress = false;
        lv_async_call(wifi_scan_done_to_ui, NULL);
    }
}
void wifi_forget_saved(void)
{
    // clear runtime
    g_wifi_ssid[0] = '\0';
    g_wifi_pass[0] = '\0';
    g_wifi_connected = false;
    s_retry_num = 0;

    // stop stack
    wifi_stop();

    // clear NVS
    settings_save_wifi_creds("", "");

    ESP_LOGW(WIFI_TAG, "Cleared saved WiFi credentials");
}

void wifi_ensure_started(void)
{
    if (!s_wifi_evgrp) {
        s_wifi_evgrp = xEventGroupCreate();
        assert(s_wifi_evgrp);
    }
    if (!s_netif_inited) {
        ESP_ERROR_CHECK(esp_netif_init());
        s_netif_inited = true;
    }

    if (!s_event_loop_inited) {
        esp_err_t e = esp_event_loop_create_default();
        if (e == ESP_OK || e == ESP_ERR_INVALID_STATE) {
            s_event_loop_inited = true;
        } else {
            ESP_ERROR_CHECK(e);
        }
    }

    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    if (!s_wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        s_wifi_inited = true;
    }

    if (!s_handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &s_wifi_any_id_inst));

        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &s_got_ip_inst));

        s_handlers_registered = true;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    esp_err_t st = esp_wifi_start();
    if (st != ESP_OK && st != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(st);
    }
}

esp_err_t wifi_start_sta(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    wifi_ensure_started();

    // If user turned WiFi off, don’t try to connect at boot/bringup.
    if (!g_wifi_on) {
        ESP_LOGW(WIFI_TAG, "wifi_start_sta ignored (wifi toggle off)");
        return ESP_ERR_INVALID_STATE;
    }

    xEventGroupClearBits(s_wifi_evgrp, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;

    esp_err_t err;

    err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        // It's fine if we're not connected yet; just log for visibility
        ESP_LOGW(WIFI_TAG, "esp_wifi_disconnect: %s", esp_err_to_name(err));
    }


    vTaskDelay(pdMS_TO_TICKS(100));

    wifi_config_t cfg = {0};
    snprintf((char*)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", ssid);
    snprintf((char*)cfg.sta.password, sizeof(cfg.sta.password), "%s", pass ? pass : "");

    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    // Optional: prevent “WPA3 only” pain (you can tune this later)
    // cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(WIFI_TAG, "wifi_start_sta connect requested (ssid=%s)", ssid);
    return ESP_OK;
}


void wifi_stop(void)
{
    g_wifi_connected = false;
    s_retry_num = 0;

    esp_wifi_disconnect();
    esp_wifi_stop();
    ESP_LOGI(WIFI_TAG, "WiFi stopped");

    if (esp_sntp_enabled()) {
        esp_sntp_stop();
        ESP_LOGI(WIFI_TAG, "SNTP stopped");
    }

    g_time_synced = false;
}

int wifi_get_rssi_dbm(int *out_rssi)
{
    if (!g_wifi_connected) return -1;

    wifi_ap_record_t ap = {0};
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err != ESP_OK) return -1;

    if (out_rssi) *out_rssi = ap.rssi;
    return 0;
}

/* ---------------- scanning ---------------- */

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

    // if picker isn't open, just ignore results
    if (!wifi_modal || !wifi_list) {
        ESP_LOGI(WIFI_TAG, "Scan done but picker not open; ignoring");
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

    wifi_ap_record_t *aprs = (wifi_ap_record_t*)calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!aprs) return;

    uint16_t n = ap_count;
    if (esp_wifi_scan_get_ap_records(&n, aprs) == ESP_OK) {
        for (int i = 0; i < n; i++) {
            wifi_add_ap_to_list((const char*)aprs[i].ssid, aprs[i].rssi, aprs[i].authmode);
        }
    }

    free(aprs);
}
