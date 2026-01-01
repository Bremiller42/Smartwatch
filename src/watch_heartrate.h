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