// watch_heartrate.c
#include <stdio.h>
#include <inttypes.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_bsp.h"
#include "esp_timer.h"
#include "watch_settings.h"
#include "lvgl.h"
#include "watch_i2c.h"
#include "watch_heartrate.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include "watch_screen_timeout.h"

#define MAX30102_ADDR           0x57
static const char *HR_TAG = "HEART";

/* =============================
 *  Scheduling controls (set from Settings later)
 * ============================= */
// How long to actively sample each cycle (ms)
uint32_t g_hr_run_ms    = 0;   // default: 10 seconds
// How often to run a cycle (ms)
uint32_t g_hr_period_ms = 0;   // default: every 60 seconds
// Enable/disable verbose raw logging (1 = on, 0 = off)
#ifndef HR_LOG_RAW
#define HR_LOG_RAW  1
#endif

// How often to print raw values when logging (ms)
#ifndef HR_LOG_RAW_EVERY_MS
#define HR_LOG_RAW_EVERY_MS  200
#endif
static int s_hr_to_token = -1;

static uint32_t HR_AC_MIN = 30;     // wrist often low amplitude
static uint32_t HR_DC_MIN = 20000;
static uint32_t HR_DC_MAX = 250000;

// BPM bounds
static float HR_BPM_MIN = 40.0f;
static float HR_BPM_MAX = 200.0f;

/* =============================
 *  MAX30102 registers (subset)
 * ============================= */
#define REG_INTR_STATUS_1       0x00
#define REG_INTR_STATUS_2       0x01
#define REG_INTR_ENABLE_1       0x02
#define REG_INTR_ENABLE_2       0x03
#define REG_FIFO_WR_PTR         0x04
#define REG_OVF_COUNTER         0x05
#define REG_FIFO_RD_PTR         0x06
#define REG_FIFO_DATA           0x07
#define REG_FIFO_CONFIG         0x08
#define REG_MODE_CONFIG         0x09
#define REG_SPO2_CONFIG         0x0A
#define REG_LED1_PA             0x0C   // RED
#define REG_LED2_PA             0x0D   // IR

/* =============================
 *  State
 * ============================= */
static bool s_max_present = false;
static TaskHandle_t s_hr_task = NULL;

/* =============================
 *  I2C helpers (NO ESP_ERROR_CHECK inside)
 * ============================= */
static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return watch_i2c_write(MAX30102_ADDR, buf, sizeof(buf), pdMS_TO_TICKS(50));
}

static esp_err_t rd(uint8_t reg, uint8_t *data, size_t len)
{
    return watch_i2c_write_read(MAX30102_ADDR, &reg, 1, data, len, pdMS_TO_TICKS(50));
}

/* “Is it there?” probe: safe 1-byte register read */
esp_err_t max30102_probe(void)
{
    uint8_t tmp = 0;
    return rd(REG_INTR_STATUS_1, &tmp, 1);
}
static hr_ui_status_t s_ui = {
    .state = HR_STATE_IDLE,
    .session_ms_total = 15000,
    .bpm_last = 0,
};
static portMUX_TYPE s_ui_mux = portMUX_INITIALIZER_UNLOCKED;

void hr_get_ui_status(hr_ui_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_ui_mux);
    *out = s_ui;
    portEXIT_CRITICAL(&s_ui_mux);
}
static void heart_enter_always_on(void)
{
    screen_keep_awake_acquire();
    screen_timeout_mark_activity();
}

static void heart_exit_always_on(void)
{
    screen_keep_awake_release();
    screen_timeout_mark_activity();
}

/* Put sensor into low-power shutdown */
void max30102_shutdown_best_effort(void)
{
    // MODE_CONFIG bit7 = SHDN
    (void)wr(REG_MODE_CONFIG, 0x80);
}
static int cmp_f(const void *a, const void *b)
{
    float fa = *(const float*)a, fb = *(const float*)b;
    return (fa > fb) - (fa < fb);
}

static float median_f(float *arr, int n)
{
    if (n <= 0) return 0;
    qsort(arr, n, sizeof(float), cmp_f);
    if (n & 1) return arr[n/2];
    return 0.5f * (arr[n/2 - 1] + arr[n/2]);
}

void hr_set_boot_bpm_current(float bpm, bool valid)
{
    portENTER_CRITICAL(&s_ui_mux);
    s_ui.bpm_current = valid ? bpm : 0.0f;
    s_ui.bpm_valid   = valid;
    // keep state idle unless you want it to appear “done”
    // s_ui.state = valid ? HR_STATE_DONE : HR_STATE_IDLE;
    portEXIT_CRITICAL(&s_ui_mux);
}

/* Init/configure sensor; returns:
 * - ESP_OK when initialized
 * - ESP_ERR_NOT_FOUND if missing/unplugged
 * - other errors for bus/protocol issues
 */
 esp_err_t max30102_init(void)
{
    esp_err_t err = max30102_probe();
    if (err != ESP_OK) return ESP_ERR_NOT_FOUND;

    // Soft reset
    err = wr(REG_MODE_CONFIG, 0x40);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(50));

    // Clear interrupts (ignore failures, but don't abort)
    uint8_t tmp;
    (void)rd(REG_INTR_STATUS_1, &tmp, 1);
    (void)rd(REG_INTR_STATUS_2, &tmp, 1);

    // Disable interrupts (polling)
    err = wr(REG_INTR_ENABLE_1, 0x00);
    if (err != ESP_OK) return err;
    err = wr(REG_INTR_ENABLE_2, 0x00);
    if (err != ESP_OK) return err;

    // FIFO config
    err = wr(REG_FIFO_CONFIG, (2 << 5));
    if (err != ESP_OK) return err;

    // Reset FIFO pointers
    err = wr(REG_FIFO_WR_PTR, 0x00);
    if (err != ESP_OK) return err;
    err = wr(REG_OVF_COUNTER, 0x00);
    if (err != ESP_OK) return err;
    err = wr(REG_FIFO_RD_PTR, 0x00);
    if (err != ESP_OK) return err;

    // SpO2 mode (RED+IR)
    err = wr(REG_MODE_CONFIG, 0x03);
    if (err != ESP_OK) return err;

    // SpO2 config: ADC range 4096nA, SR 100Hz, PW 411us
    err = wr(REG_SPO2_CONFIG, 0x33);
    if (err != ESP_OK) return err;

    // LED currents (tune later)
    err = wr(REG_LED1_PA, 0x24); // RED
    if (err != ESP_OK) return err;
    err = wr(REG_LED2_PA, 0x24); // IR
    if (err != ESP_OK) return err;

    ESP_LOGI(HR_TAG, "MAX30102 init OK");
    return ESP_OK;
}

esp_err_t max30102_read_sample(max30102_sample_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    uint8_t d[6];
    esp_err_t err = rd(REG_FIFO_DATA, d, sizeof(d));
    if (err != ESP_OK) return err;

    uint32_t red = ((uint32_t)d[0] << 16) | ((uint32_t)d[1] << 8) | d[2];
    uint32_t ir  = ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 8) | d[5];

    red &= 0x3FFFF;
    ir  &= 0x3FFFF;

    out->red = red;
    out->ir  = ir;
    return ESP_OK;
}
typedef struct {
    // DC estimate (IIR low-pass)
    float dc;

    // Smoothed AC (after DC removal)
    float ac_smooth;

    // For peak detection
    float prev;
    float prev2;

    // amplitude tracking
    float ac_abs_avg;
} ppg_filter_t;

static void ppg_filter_reset(ppg_filter_t *f, float init_dc)
{
    f->dc = init_dc;
    f->ac_smooth = 0;
    f->prev = 0;
    f->prev2 = 0;
    f->ac_abs_avg = 0;
}

// returns filtered AC value
static float ppg_filter_step(ppg_filter_t *f, float x)
{
    // 1) DC low-pass (slow)
    // alpha_dc closer to 1 => slower DC tracking (good)
    const float alpha_dc = 0.995f;
    f->dc = alpha_dc * f->dc + (1.0f - alpha_dc) * x;

    // 2) AC component
    float ac = x - f->dc;

    // 3) Smooth AC a bit (low-pass)
    const float alpha_lp = 0.85f;
    f->ac_smooth = alpha_lp * f->ac_smooth + (1.0f - alpha_lp) * ac;

    // 4) Track average abs amplitude (for “signal quality”)
    float abs_ac = (f->ac_smooth < 0) ? -f->ac_smooth : f->ac_smooth;
    const float alpha_amp = 0.95f;
    f->ac_abs_avg = alpha_amp * f->ac_abs_avg + (1.0f - alpha_amp) * abs_ac;

    return f->ac_smooth;
}

typedef struct {
    int peaks;
    uint32_t last_peak_ms;
    float bpm;          // latest estimate
    float bpm_avg;      // smoothed
    bool bpm_valid;
} hr_est_t;

static void hr_est_reset(hr_est_t *h)
{
    h->peaks = 0;
    h->last_peak_ms = 0;
    h->bpm = 0;
    h->bpm_avg = 0;
    h->bpm_valid = false;
}

// very simple local-max peak detection on AC waveform
static void hr_process_sample(hr_est_t *h, ppg_filter_t *f, float ac, uint32_t now_ms)
{
    float a = f->prev2;
    float b = f->prev;
    float c = ac;

    float thr = (float)HR_AC_MIN;
    float adaptive = 0.60f * f->ac_abs_avg;
    if (adaptive > thr) thr = adaptive;

    const uint32_t refractory_ms = 250;

    bool is_local_max = (b > a && b > c);
    bool above_thr = (b > thr);

    if (is_local_max && above_thr) {
        if (h->last_peak_ms == 0 || (now_ms - h->last_peak_ms) >= refractory_ms) {

            if (h->last_peak_ms != 0) {
                uint32_t dt = now_ms - h->last_peak_ms;
                float bpm = 60000.0f / (float)dt;

                if (bpm >= HR_BPM_MIN && bpm <= HR_BPM_MAX) {
                    h->bpm = bpm;
                    h->bpm_avg = (h->bpm_avg <= 0.1f) ? bpm : (0.85f*h->bpm_avg + 0.15f*bpm);
                    h->bpm_valid = true;
                } else {
                    h->bpm_valid = false;
                }
            }

            h->last_peak_ms = now_ms;
            h->peaks++;
        }
    }

    // shift history LAST
    f->prev2 = f->prev;
    f->prev = ac;
}

/* =============================
 *  Public: Read-now trigger (UI button calls this)
 * ============================= */
void hr_request_read_now(void)
{
    if (s_hr_task) {
        heart_enter_always_on();
        xTaskNotifyGive(s_hr_task);

    }
}

/* =============================
 *  Task: windowed sampling
 * ============================= */
static void max_task(void *arg)
{
    (void)arg;
    s_hr_task = xTaskGetCurrentTaskHandle();


    // init I2C once
    esp_err_t err = watch_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGE(HR_TAG, "I2C init failed: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Timing knobs
    const TickType_t idle_poll = pdMS_TO_TICKS(250);   // responsiveness while idle
    const TickType_t retry_delay = pdMS_TO_TICKS(1000);

    // log rate limit
    uint32_t last_warn_ms = 0;

    // schedule anchor
    TickType_t last_run_start = xTaskGetTickCount();

    while (1) {
         // OFF/manual mode: wait here until user taps "Read Now"
    if (g_hr_period_ms == 0 || g_hr_run_ms == 0) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    } else {
        // Scheduled mode: wait until next period OR read-now notify
        TickType_t period_ticks = pdMS_TO_TICKS(g_hr_period_ms);

        TickType_t now = xTaskGetTickCount();
        TickType_t next_due = last_run_start + period_ticks;
        TickType_t wait_ticks = (next_due > now) ? (next_due - now) : 0;

        while (wait_ticks > 0) {
            TickType_t chunk = (wait_ticks > idle_poll) ? idle_poll : wait_ticks;

            if (ulTaskNotifyTake(pdTRUE, chunk) > 0) {
                ESP_LOGI(HR_TAG, "Read-now requested (scheduled mode)");
                break;
            }

            now = xTaskGetTickCount();
            next_due = last_run_start + period_ticks;
            wait_ticks = (next_due > now) ? (next_due - now) : 0;
        }

        last_run_start = xTaskGetTickCount();
    }
        // ---------- START WINDOW ----------
        err = max30102_init();
        if (err != ESP_OK) {
            s_max_present = false;

            uint32_t tms = esp_log_timestamp();
            if (tms - last_warn_ms > 3000) {
                last_warn_ms = tms;
                heart_exit_always_on();

                ESP_LOGW(HR_TAG, "MAX not present (init: %s). Will retry.", esp_err_to_name(err));
            }

            // Don’t hammer the bus
            vTaskDelay(retry_delay);
            continue;
        }

        s_max_present = true;
        // --- Warmup / settle ---
        // Drain a few samples and initialize DC to a real value
        ppg_filter_t filt;
        hr_est_t hr;
        hr_est_reset(&hr);

        max30102_sample_t warm;
        uint32_t good = 0;
        uint32_t ir_sum = 0;

        // grab ~25 good samples (~0.5s at 50Hz) to find a stable starting DC
        for (int i = 0; i < 25; i++) {
            if (max30102_read_sample(&warm) == ESP_OK && warm.ir > 1000) {
                ir_sum += warm.ir;
                good++;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        uint32_t ir0 = (good > 0) ? (ir_sum / good) : (HR_DC_MIN + 1000);
        ppg_filter_reset(&filt, (float)ir0);

        // uint32_t next_log_ms = esp_log_timestamp();

        // session knobs
        const uint32_t stabilize_need_ms = 2000; // 2s stable before measuring
        const uint32_t session_total_ms  = (g_hr_run_ms ? g_hr_run_ms : 15000);

        uint32_t good_ms = 0;
        bool measuring = false;

        float bpm_samples[32];
        int bpm_n = 0;
        uint32_t start_ms = esp_log_timestamp();
        uint32_t next_ui_ms = esp_log_timestamp();   // next time we’re allowed to poke UI

        for (;;) {
            // end conditions:
            uint32_t now_ms = esp_log_timestamp();
            uint32_t elapsed_ms = now_ms - start_ms;

            // read sample @ 50Hz
            max30102_sample_t s;
            err = max30102_read_sample(&s);
            if (err != ESP_OK) {
                s_max_present = false;
                    portENTER_CRITICAL(&s_ui_mux);
                    s_ui.state = HR_STATE_ERROR;      // or HR_STATE_IDLE if you prefer
                    s_ui.bpm_valid = false;
                    s_ui.bpm_current = 0;
                    portEXIT_CRITICAL(&s_ui_mux);

                
                ui_hr_widget_refresh_request(); // immediate on error
                heart_exit_always_on();
                ESP_LOGW(HR_TAG, "read failed: %s", esp_err_to_name(err));
                break;
            }

            float ac = ppg_filter_step(&filt, (float)s.ir);

            bool dc_ok  = (filt.dc >= HR_DC_MIN && filt.dc <= HR_DC_MAX);
            bool amp_ok = (filt.ac_abs_avg >= HR_AC_MIN);

            if (dc_ok) hr_process_sample(&hr, &filt, ac, now_ms);
            else hr.bpm_valid = false;

            bool bpm_ok = hr.bpm_valid;
            bool signal_ok = (dc_ok && amp_ok);

            // ---- update shared UI status (state + progress) ----
            portENTER_CRITICAL(&s_ui_mux);
            s_ui.contact_ok = dc_ok;
            s_ui.signal_ok  = signal_ok;

            if (!measuring) {
                // pre-measure phase
                if (!dc_ok) s_ui.state = HR_STATE_SEEK_CONTACT;
                else if (!amp_ok) s_ui.state = HR_STATE_STABILIZING;
                else s_ui.state = HR_STATE_STABILIZING;
                s_ui.session_ms_total = session_total_ms;
                s_ui.session_ms_elapsed = 0;
                s_ui.bpm_valid = false;
                s_ui.bpm_current = 0;
            } else {
                s_ui.state = HR_STATE_MEASURING;
                s_ui.session_ms_total = session_total_ms;
                s_ui.session_ms_elapsed = elapsed_ms; // (elapsed since start_ms; see note below)
                s_ui.bpm_valid = bpm_ok;
                s_ui.bpm_current = (bpm_ok ? hr.bpm_avg : 0.0f);

            }
            portEXIT_CRITICAL(&s_ui_mux);

            // ---- stability gate ----
            if (!measuring) {
                if (signal_ok) good_ms += 20;
                else good_ms = 0;

                if (good_ms >= stabilize_need_ms) {
                    measuring = true;
                    // reset session clock so progress is “measuring time only”
                    start_ms = esp_log_timestamp();
                    elapsed_ms = 0;
                    bpm_n = 0;
                }
            } else {
                // ---- collect bpm samples during measuring ----
                if (signal_ok && bpm_ok && bpm_n < (int)(sizeof(bpm_samples)/sizeof(bpm_samples[0]))) {
                    bpm_samples[bpm_n++] = hr.bpm_avg;
                }

                // end session
                if (elapsed_ms >= session_total_ms) {
                    float bpm_final = (bpm_n >= 5) ? median_f(bpm_samples, bpm_n) : hr.bpm_avg;

                    portENTER_CRITICAL(&s_ui_mux);
                    s_ui.state = HR_STATE_DONE;
                    s_ui.bpm_valid = (bpm_final > 0.1f);
                    s_ui.bpm_current = bpm_final;
                    if (s_ui.bpm_valid) s_ui.bpm_last = bpm_final;
                    s_ui.session_ms_elapsed = session_total_ms;
                    portEXIT_CRITICAL(&s_ui_mux);
                    settings_save_hr_current(bpm_final, (bpm_final > 0.1f));
                    
                    ui_hr_widget_refresh_request();

                    break;
                }
            }

            // UI refresh
            // UI refresh (throttled to ~6-7 Hz)
            uint32_t t_ui = esp_log_timestamp();
            if ((int32_t)(t_ui - next_ui_ms) >= 0) {
                ui_hr_widget_refresh_request();
                next_ui_ms = t_ui + 150;   // 150ms
            }

            vTaskDelay(pdMS_TO_TICKS(20));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        heart_exit_always_on();

        // END WINDOW: shutdown sensor to save power
        max30102_shutdown_best_effort();
        ESP_LOGI(HR_TAG, "HR window complete");
    } // ✅ closes while(1)
       

}     // ✅ closes max_task()


void start_max30102_task(void)
{
    xTaskCreatePinnedToCore(max_task, "max30102", 4096, NULL, 5, NULL, 1);
}
