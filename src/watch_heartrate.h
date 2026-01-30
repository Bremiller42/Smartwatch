// watch_max30102.h
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    uint32_t red;
    uint32_t ir;
} max30102_sample_t;

esp_err_t max30102_init(void);
esp_err_t max30102_read_sample(max30102_sample_t *out);  // polls FIFO (1 sample)
void start_max30102_task(void);
extern uint32_t g_hr_run_ms;
extern uint32_t g_hr_period_ms;

void start_max30102_task(void);
void hr_request_read_now(void);
typedef enum {
    HR_STATE_IDLE = 0,
    HR_STATE_SEEK_CONTACT,
    HR_STATE_STABILIZING,
    HR_STATE_MEASURING,
    HR_STATE_DONE,
    HR_STATE_ERROR,
} hr_state_t;

typedef struct {
    hr_state_t state;

    // live quality
    bool contact_ok;
    bool signal_ok;

    // session info
    uint32_t session_ms_total;     // e.g. 15000
    uint32_t session_ms_elapsed;

    // results
    bool bpm_valid;
    float bpm_current;             // session result while measuring/done
    float bpm_last;                // previous session result
} hr_ui_status_t;

void hr_get_ui_status(hr_ui_status_t *out);   // thread-safe copy
void ui_hr_widget_refresh_request(void);
void hr_set_boot_bpm_current(float bpm, bool valid);
bool hr_allowed_by_power(void);
