// FILE: src/watch_weather.h
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEATHER_LOC_MODE_ZIP = 0,
    WEATHER_LOC_MODE_IP  = 1,
} weather_loc_mode_t;

typedef struct {
    bool     valid;
    int      temp_c_x10;       // Celsius * 10
    int      humidity_pct;     // 0-100
    int      condition_id;     // OpenWeather weather[0].id
    int      wind_mps_x10;     // wind speed (m/s) * 10
    bool     is_day;           // computed from dt vs sunrise/sunset
    uint32_t updated_ms;       // esp_timer_get_time()/1000 at fetch time
} weather_snapshot_t;

typedef struct {
    bool valid;
    bool inflight;
    bool loc_ok;
    int  last_http_status;
    int  last_http_err;   // esp_err_t stored as int for header cleanliness
    uint32_t updated_ms;
} weather_diag_t;

bool weather_get_diag(weather_diag_t *out);

typedef void (*weather_update_cb_t)(const weather_snapshot_t *snap);

/* ---- Init/config ---- */
void weather_init(void);

/* Set OpenWeather API key (must be set before fetch). Stored in RAM only by default. */
void weather_set_api_key(const char *key);

/* Location controls (persisted in NVS) */
bool weather_set_zip(const char *zip5);                 // "90210"
bool weather_get_zip(char *out_zip, int out_len);
void weather_set_loc_mode(weather_loc_mode_t mode);
weather_loc_mode_t weather_get_loc_mode(void);

/* Optional: units control (persisted) */
void weather_set_use_fahrenheit(bool use_f);
bool weather_get_use_fahrenheit(void);

/* ---- Fetch control ---- */
void weather_request_update(void);                      // async fetch (task)
bool weather_get_latest(weather_snapshot_t *out);       // snapshot copy
void weather_register_cb(weather_update_cb_t cb);       // called on update (from weather task context)

/* ---- Suggested periodic scheduling ---- */
void weather_set_min_refresh_ms(uint32_t ms);           // default 15 min
uint32_t weather_get_min_refresh_ms(void);

// Returns a short status string for UI (e.g., "Never", "2m ago", "Stale", "No WiFi")
bool weather_get_status_text(char *out, int out_len);


#ifdef __cplusplus
}
#endif
