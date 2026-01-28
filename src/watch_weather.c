// FILE: src/watch_weather.c
#include "watch_weather.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "secrets/keys.h"
#include "watch_audio.h"
#include "esp_heap_caps.h"

// HTTPS / cert bundle
#include "esp_crt_bundle.h"

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "lwip/netdb.h"

static const char *TAG = "WEATHER";

/* -------- NVS keys (ns="watch") -------- */
#define NVS_NS                 "watch"
#define KEY_W_ZIP              "w_zip"
#define KEY_W_MODE             "w_mode"
#define KEY_W_USE_F            "w_use_f"
#define KEY_W_MINREFRESH       "w_minref"

/* -------- Quota (ns="watch") -------- */
#define KEY_W_DAY              "w_day"
#define KEY_W_COUNT            "w_cnt"
#define WEATHER_REQ_LIMIT_DAY  1000

/* -------- Event bits -------- */
#define EV_REQ_UPDATE          (1 << 0)

/* -------- Defaults -------- */
#define DEFAULT_MIN_REFRESH_MS (15UL * 60UL * 1000UL)   // 15 min
#define HTTP_TIMEOUT_MS        9000
#define HTTP_RX_MAX            2048

/* -------- State -------- */
static EventGroupHandle_t s_ev = NULL;
static TaskHandle_t       s_task = NULL;

static const char         *s_api_key = NULL;

static weather_loc_mode_t s_mode = WEATHER_LOC_MODE_ZIP;
static char               s_zip[16] = "27592";
static bool               s_use_f = true;

static uint32_t           s_min_refresh_ms = DEFAULT_MIN_REFRESH_MS;
static uint32_t           s_last_fetch_ms = 0;

static weather_snapshot_t s_last = {0};

#define WEATHER_MAX_SUBS 4
static weather_update_cb_t s_cbs[WEATHER_MAX_SUBS] = {0};
static int s_cbs_len = 0;

// Diagnostics
static int       s_last_http_status = 0;
static int       s_last_http_errno  = 0;
static esp_err_t s_last_http_err    = ESP_OK;
static bool      s_last_loc_ok      = false;
static bool      s_fetch_inflight   = false;

// Quota state
static uint32_t  s_quota_day   = 0;
static uint32_t  s_quota_count = 0;

/* -------- Helpers -------- */
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void nvs_load_u32(nvs_handle_t h, const char *key, uint32_t *v, uint32_t defv)
{
    uint32_t tmp = 0;
    esp_err_t e = nvs_get_u32(h, key, &tmp);
    *v = (e == ESP_OK) ? tmp : defv;
}

static void nvs_load_i32(nvs_handle_t h, const char *key, int32_t *v, int32_t defv)
{
    int32_t tmp = 0;
    esp_err_t e = nvs_get_i32(h, key, &tmp);
    *v = (e == ESP_OK) ? tmp : defv;
}

static void nvs_load_str(nvs_handle_t h, const char *key, char *out, size_t out_len, const char *defv)
{
    size_t need = 0;
    esp_err_t e = nvs_get_str(h, key, NULL, &need);
    if (e == ESP_OK && need > 0 && need < out_len) {
        e = nvs_get_str(h, key, out, &need);
        if (e == ESP_OK) return;
    }
    snprintf(out, out_len, "%s", defv);
}

static void nvs_save_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_i32(const char *key, int32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_u32(const char *key, uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

/* -------- Quota --------
   Uses "epoch day" if time is valid; otherwise uses "uptime day" with a marker bit.
   This prevents accidental mixing of uptime buckets with real epoch buckets.
*/
static uint32_t quota_day_bucket(void)
{
    time_t epoch = 0;
    time(&epoch);

    // If SNTP hasn't set time yet, epoch will be small.
    // Use uptime-based day bucket with high-bit marker so it won't collide with real epoch days.
    if (epoch < 1700000000) { // ~2023-11, "time not set" heuristic
        uint32_t up_day = now_ms() / (24UL * 60UL * 60UL * 1000UL);
        return 0x80000000UL | (up_day & 0x7FFFFFFFUL);
    }

    return (uint32_t)(epoch / 86400);
}

static void quota_load_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    nvs_load_u32(h, KEY_W_DAY, &s_quota_day, 0);
    nvs_load_u32(h, KEY_W_COUNT, &s_quota_count, 0);
    nvs_close(h);
}

static void quota_sync_day_if_needed(void)
{
    uint32_t day = quota_day_bucket();
    if (day != s_quota_day) {
        s_quota_day = day;
        s_quota_count = 0;
        nvs_save_u32(KEY_W_DAY, s_quota_day);
        nvs_save_u32(KEY_W_COUNT, s_quota_count);
        ESP_LOGI(TAG, "quota: new day bucket=0x%08lx count reset", (unsigned long)s_quota_day);
    }
}

static bool quota_try_consume(uint32_t n, uint32_t *out_remaining)
{
    quota_sync_day_if_needed();

    if (s_quota_count >= WEATHER_REQ_LIMIT_DAY) {
        if (out_remaining) *out_remaining = 0;
        return false;
    }

    uint32_t left = WEATHER_REQ_LIMIT_DAY - s_quota_count;
    if (n > left) {
        if (out_remaining) *out_remaining = left;
        return false;
    }

    s_quota_count += n;
    nvs_save_u32(KEY_W_COUNT, s_quota_count);

    if (out_remaining) *out_remaining = (WEATHER_REQ_LIMIT_DAY - s_quota_count);
    return true;
}

/* ---------------- HTTP(S) GET ----------------
   - Works with http:// and https://
   - Uses ESP-IDF certificate bundle for TLS trust
   - Allows redirects
   - Enforces daily hard limit (1000 requests/day)
*/
static bool http_get_to_buf(const char *url, char *out, size_t out_len, int *out_status)
{
    if (!url || !out || out_len < 8) return false;
    out[0] = 0;

    // Clear last error/status for better diagnostics
    s_last_http_status = 0;
    s_last_http_errno  = 0;
    s_last_http_err    = ESP_OK;

    // Quota check (each HTTP call costs 1 token)
    uint32_t remaining = 0;
    if (!quota_try_consume(1, &remaining)) {
        s_last_http_err = ESP_ERR_INVALID_STATE;
        s_last_http_status = 429;
        if (out_status) *out_status = 429;
        ESP_LOGW(TAG, "quota: DAILY LIMIT HIT (%u/%u). Blocking request url=%s",
                 (unsigned)s_quota_count, (unsigned)WEATHER_REQ_LIMIT_DAY, url);
        return false;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .keep_alive_enable = false,

        // Allow redirects (important for some geo services)
        .disable_auto_redirect = false,

        // Bigger internal buffers help stability on TLS + chunked responses
        .buffer_size = 4096,
        .buffer_size_tx = 1024,

        // HTTPS trust: use ESP-IDF certificate bundle
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        s_last_http_err = ESP_FAIL;
        return false;
    }

    esp_http_client_set_method(c, HTTP_METHOD_GET);
    esp_http_client_set_header(c, "User-Agent", "NoesisWatch/1.0");
    esp_http_client_set_header(c, "Connection", "close");

    // Open connection and send request
    esp_err_t e = esp_http_client_open(c, 0);
    if (e != ESP_OK) {
        s_last_http_err = e;
        s_last_http_errno = esp_http_client_get_errno(c);
        ESP_LOGE(TAG, "HTTP open failed: %s errno=%d url=%s (quota_rem=%u)",
                 esp_err_to_name(e), s_last_http_errno, url, (unsigned)remaining);
        esp_http_client_cleanup(c);
        return false;
    }

    // Fetch headers
    int64_t cl = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    bool chunked = esp_http_client_is_chunked_response(c);

    s_last_http_status = status;
    s_last_http_errno = esp_http_client_get_errno(c);
    if (out_status) *out_status = status;

    ESP_LOGI(TAG, "HTTP status=%d content_len=%lld chunked=%d errno=%d quota_rem=%u url=%s",
             status, (long long)cl, (int)chunked, s_last_http_errno, (unsigned)remaining, url);

    // Read body
    size_t total = 0;
    while (total < out_len - 1) {
        int r = esp_http_client_read(c, out + total, (int)(out_len - 1 - total));
        if (r < 0) {
            s_last_http_errno = esp_http_client_get_errno(c);
            ESP_LOGE(TAG, "HTTP read failed r=%d errno=%d url=%s", r, s_last_http_errno, url);
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            return false;
        }
        if (r == 0) break;
        total += (size_t)r;
    }
    out[total] = 0;

    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    // Only 2xx is success
    if (!(status >= 200 && status < 300)) {
        ESP_LOGE(TAG, "HTTP non-2xx: %d body_len=%u url=%s",
                 status, (unsigned)total, url);
        return false;
    }
    if (total == 0) {
        ESP_LOGE(TAG, "HTTP 2xx but empty body url=%s", url);
        return false;
    }

    return true;
}

/* -------- Location resolution --------
   A) ZIP -> OpenWeather geocode: /geo/1.0/zip?zip=...&appid=...
   B) IP  -> ipapi.co/json -> lat/lon
*/
static bool resolve_latlon_from_zip(float *out_lat, float *out_lon)
{
    char url[256];
    char rx[HTTP_RX_MAX];
    int status = 0;

    snprintf(url, sizeof(url),
             "https://api.openweathermap.org/geo/1.0/zip?zip=%s,US&appid=%s",
             s_zip, s_api_key);

    if (!http_get_to_buf(url, rx, sizeof(rx), &status)) {
        ESP_LOGE(TAG, "ZIP geocode failed (HTTP %d err=%s errno=%d)",
                 status, esp_err_to_name(s_last_http_err), s_last_http_errno);
        return false;
    }

    cJSON *root = cJSON_Parse(rx);
    if (!root) return false;

    cJSON *lat = cJSON_GetObjectItemCaseSensitive(root, "lat");
    cJSON *lon = cJSON_GetObjectItemCaseSensitive(root, "lon");

    bool ok = cJSON_IsNumber(lat) && cJSON_IsNumber(lon);
    if (ok) {
        *out_lat = (float)lat->valuedouble;
        *out_lon = (float)lon->valuedouble;
    }

    cJSON_Delete(root);
    return ok;
}

static bool resolve_latlon_from_ip(float *out_lat, float *out_lon)
{
    char rx[HTTP_RX_MAX];
    int status = 0;

    if (!http_get_to_buf("https://ipapi.co/json/", rx, sizeof(rx), &status)) {
        ESP_LOGE(TAG, "IP geoloc failed (HTTP %d err=%s errno=%d)",
                 status, esp_err_to_name(s_last_http_err), s_last_http_errno);
        return false;
    }

    cJSON *root = cJSON_Parse(rx);
    if (!root) return false;

    cJSON *lat = cJSON_GetObjectItemCaseSensitive(root, "latitude");
    cJSON *lon = cJSON_GetObjectItemCaseSensitive(root, "longitude");

    bool ok = cJSON_IsNumber(lat) && cJSON_IsNumber(lon);
    if (ok) {
        *out_lat = (float)lat->valuedouble;
        *out_lon = (float)lon->valuedouble;
    }

    cJSON_Delete(root);
    return ok;
}

/* -------- Weather fetch -------- */
static bool fetch_weather_by_latlon(float lat, float lon, weather_snapshot_t *out)
{
    char url[320];
    char rx[HTTP_RX_MAX];
    int status = 0;

    snprintf(url, sizeof(url),
             "https://api.openweathermap.org/data/2.5/weather?lat=%.5f&lon=%.5f&units=metric&appid=%s",
             (double)lat, (double)lon, s_api_key);

    if (!http_get_to_buf(url, rx, sizeof(rx), &status)) {
        ESP_LOGE(TAG, "Weather fetch failed (HTTP %d err=%s errno=%d)",
                 status, esp_err_to_name(s_last_http_err), s_last_http_errno);
        return false;
    }

    cJSON *root = cJSON_Parse(rx);
    if (!root) return false;

    cJSON *main = cJSON_GetObjectItemCaseSensitive(root, "main");
    cJSON *temp = main ? cJSON_GetObjectItemCaseSensitive(main, "temp") : NULL;
    cJSON *hum  = main ? cJSON_GetObjectItemCaseSensitive(main, "humidity") : NULL;

    cJSON *weather = cJSON_GetObjectItemCaseSensitive(root, "weather");
    cJSON *w0 = (cJSON_IsArray(weather) ? cJSON_GetArrayItem(weather, 0) : NULL);
    cJSON *id = w0 ? cJSON_GetObjectItemCaseSensitive(w0, "id") : NULL;

    cJSON *wind = cJSON_GetObjectItemCaseSensitive(root, "wind");
    cJSON *ws   = wind ? cJSON_GetObjectItemCaseSensitive(wind, "speed") : NULL; // m/s

    cJSON *dtj  = cJSON_GetObjectItemCaseSensitive(root, "dt"); // unix seconds
    cJSON *sys  = cJSON_GetObjectItemCaseSensitive(root, "sys");
    cJSON *sunr = sys ? cJSON_GetObjectItemCaseSensitive(sys, "sunrise") : NULL;
    cJSON *suns = sys ? cJSON_GetObjectItemCaseSensitive(sys, "sunset")  : NULL;

    bool ok = cJSON_IsNumber(temp) && cJSON_IsNumber(hum) && cJSON_IsNumber(id);

    if (ok) {
        memset(out, 0, sizeof(*out));
        out->valid = true;

        out->temp_c_x10     = (int)(temp->valuedouble * 10.0);
        out->humidity_pct   = hum->valueint;
        out->condition_id   = id->valueint;
        out->updated_ms     = now_ms();

        if (cJSON_IsNumber(ws)) out->wind_mps_x10 = (int)(ws->valuedouble * 10.0);
        else out->wind_mps_x10 = 0;

        bool is_day = true;
        if (cJSON_IsNumber(dtj) && cJSON_IsNumber(sunr) && cJSON_IsNumber(suns)) {
            int64_t dt  = (int64_t)dtj->valuedouble;
            int64_t sr  = (int64_t)sunr->valuedouble;
            int64_t ss  = (int64_t)suns->valuedouble;
            is_day = (dt >= sr && dt < ss);
        }
        out->is_day = is_day;
    }

    cJSON_Delete(root);
    return ok;
}

/* -------- Task -------- */
static void weather_task(void *arg)
{
    (void)arg;

    for (;;) {
        xEventGroupWaitBits(s_ev, EV_REQ_UPDATE, pdTRUE, pdFALSE, portMAX_DELAY);

        s_fetch_inflight = true;
        s_last_loc_ok = false;

        if (!s_api_key || s_api_key[0] == 0) {
            ESP_LOGW(TAG, "API key not set; skipping");
            s_fetch_inflight = false;
            continue;
        }

        uint32_t t = now_ms();
        if (s_last_fetch_ms && (t - s_last_fetch_ms) < s_min_refresh_ms) {
            uint32_t wait_ms = s_min_refresh_ms - (t - s_last_fetch_ms);
            ESP_LOGI(TAG, "Throttled (min refresh %ums; remaining %ums)",
                     (unsigned)s_min_refresh_ms, (unsigned)wait_ms);
            s_fetch_inflight = false;
            continue;
        }

        float lat = 0.0f, lon = 0.0f;
        bool loc_ok = (s_mode == WEATHER_LOC_MODE_ZIP)
                        ? resolve_latlon_from_zip(&lat, &lon)
                        : resolve_latlon_from_ip(&lat, &lon);

        if (!loc_ok) {
            ESP_LOGE(TAG, "Location resolve failed (mode=%d zip=%s) http=%d err=%s errno=%d",
                     (int)s_mode, s_zip, s_last_http_status,
                     esp_err_to_name(s_last_http_err), s_last_http_errno);
            s_fetch_inflight = false;
            continue;
        }
        s_last_loc_ok = true;

        ESP_LOGI(TAG, "loc: mode=%d zip=%s -> lat=%.5f lon=%.5f",
                 (int)s_mode, s_zip, (double)lat, (double)lon);

        weather_snapshot_t snap = {0};
        if (!fetch_weather_by_latlon(lat, lon, &snap)) {
            ESP_LOGE(TAG, "Weather fetch failed after loc ok (http=%d err=%s errno=%d)",
                     s_last_http_status, esp_err_to_name(s_last_http_err), s_last_http_errno);
            s_fetch_inflight = false;
            continue;
        }

        s_last_fetch_ms = t;
        s_last = snap;

        ESP_LOGI(TAG, "Updated: %d.%dC, hum=%d%%, id=%d wind=%d.%dm/s day=%d quota=%u/%u",
                 s_last.temp_c_x10 / 10, abs(s_last.temp_c_x10 % 10),
                 s_last.humidity_pct, s_last.condition_id,
                 s_last.wind_mps_x10 / 10, abs(s_last.wind_mps_x10 % 10),
                 (int)s_last.is_day,
                 (unsigned)s_quota_count, (unsigned)WEATHER_REQ_LIMIT_DAY);

        watch_audio_beep(1300, 50);

        for (int i = 0; i < s_cbs_len; i++) {
            if (s_cbs[i]) s_cbs[i](&s_last);
        }

        s_fetch_inflight = false;
    }
}

bool weather_get_diag(weather_diag_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->valid = s_last.valid;
    out->inflight = s_fetch_inflight;
    out->loc_ok = s_last_loc_ok;
    out->last_http_status = s_last_http_status;
    out->last_http_err = (int)s_last_http_err;
    out->updated_ms = s_last.updated_ms;
    return true;
}

/* -------- Public API -------- */
void weather_init(void)
{
    ESP_LOGI(TAG, "Initializing weather module");
    weather_set_api_key(OW_API_KEY);
    ESP_LOGI(TAG, "API key set");

    // Load quota first so we don't spam after reboot
    quota_load_from_nvs();
    quota_sync_day_if_needed();

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_load_str(h, KEY_W_ZIP, s_zip, sizeof(s_zip), "27592");

        int32_t mode = 0;
        nvs_load_i32(h, KEY_W_MODE, &mode, (int32_t)WEATHER_LOC_MODE_ZIP);
        if (mode != WEATHER_LOC_MODE_ZIP && mode != WEATHER_LOC_MODE_IP) mode = WEATHER_LOC_MODE_ZIP;
        s_mode = (weather_loc_mode_t)mode;

        int32_t usef = 1;
        nvs_load_i32(h, KEY_W_USE_F, &usef, 1);
        s_use_f = (usef != 0);

        nvs_load_u32(h, KEY_W_MINREFRESH, &s_min_refresh_ms, DEFAULT_MIN_REFRESH_MS);

        nvs_close(h);
    }

    if (!s_ev) s_ev = xEventGroupCreate();

    if (!s_task) {
        xTaskCreate(weather_task, "weather_task", 8192, NULL, 5, &s_task);
    }

    ESP_LOGI(TAG, "init: mode=%d zip=%s use_f=%d min_refresh=%u quota=%u/%u day=0x%08lx",
             (int)s_mode, s_zip, (int)s_use_f, (unsigned)s_min_refresh_ms,
             (unsigned)s_quota_count, (unsigned)WEATHER_REQ_LIMIT_DAY,
             (unsigned long)s_quota_day);
}

void weather_set_api_key(const char *key)
{
    s_api_key = key;
}

bool weather_set_zip(const char *zip5)
{
    if (!zip5) return false;

    int n = 0;
    for (const char *p = zip5; *p; p++) {
        if (!isdigit((unsigned char)*p)) return false;
        n++;
    }
    if (n != 5) return false;

    snprintf(s_zip, sizeof(s_zip), "%s", zip5);
    nvs_save_str(KEY_W_ZIP, s_zip);
    return true;
}

bool weather_get_zip(char *out_zip, int out_len)
{
    if (!out_zip || out_len <= 0) return false;
    snprintf(out_zip, out_len, "%s", s_zip);
    return true;
}

void weather_set_loc_mode(weather_loc_mode_t mode)
{
    if (mode != WEATHER_LOC_MODE_ZIP && mode != WEATHER_LOC_MODE_IP) return;
    s_mode = mode;
    nvs_save_i32(KEY_W_MODE, (int32_t)mode);
}

weather_loc_mode_t weather_get_loc_mode(void)
{
    return s_mode;
}

void weather_set_use_fahrenheit(bool use_f)
{
    s_use_f = use_f;
    nvs_save_i32(KEY_W_USE_F, (int32_t)(use_f ? 1 : 0));
}

bool weather_get_use_fahrenheit(void)
{
    return s_use_f;
}

void weather_request_update(void)
{
    if (!s_ev) return;

    // Dedup: only set the bit if it isn't already set.
    EventBits_t bits = xEventGroupGetBits(s_ev);
    if (bits & EV_REQ_UPDATE) {
        ESP_LOGI(TAG, "Weather update requested (dedup: already pending)");
        return;
    }

    ESP_LOGI(TAG, "Weather update requested");

    // Useful for TLS debugging: TLS cert validation needs real wall-clock time.
    time_t epoch = 0;
    time(&epoch);
    ESP_LOGI(TAG, "time: epoch=%ld uptime_us=%lld quota=%u/%u day=0x%08lx",
             (long)epoch, (long long)esp_timer_get_time(),
             (unsigned)s_quota_count, (unsigned)WEATHER_REQ_LIMIT_DAY,
             (unsigned long)quota_day_bucket());

    xEventGroupSetBits(s_ev, EV_REQ_UPDATE);
}

bool weather_get_latest(weather_snapshot_t *out)
{
    if (!out) return false;
    *out = s_last;
    return out->valid;
}

void weather_register_cb(weather_update_cb_t cb)
{
    if (!cb) return;

    for (int i = 0; i < s_cbs_len; i++) {
        if (s_cbs[i] == cb) return;
    }

    if (s_cbs_len < WEATHER_MAX_SUBS) {
        s_cbs[s_cbs_len++] = cb;
        return;
    }

    ESP_LOGW(TAG, "weather_register_cb: subscriber list full");
}

void weather_set_min_refresh_ms(uint32_t ms)
{
    if (ms < 300UL * 1000UL) ms = 300UL * 1000UL; // clamp >= 5 min
    s_min_refresh_ms = ms;
    nvs_save_u32(KEY_W_MINREFRESH, ms);
}

uint32_t weather_get_min_refresh_ms(void)
{
    return s_min_refresh_ms;
}
