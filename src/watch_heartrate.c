// watch_max30102.c
#include <stdio.h>
#include <inttypes.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_bsp.h"
#include "esp_timer.h"

#include "lvgl.h"
#include "watch_ui.h"      // ui_get_hr_debug_lbl()
#include "watch_i2c.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

typedef struct {
    uint32_t red;
    uint32_t ir;
} max30102_sample_t;

/* =============================
 *  State
 * ============================= */
static uint32_t g_red = 0, g_ir = 0;
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
static esp_err_t max30102_probe(void)
{
    uint8_t tmp = 0;
    return rd(REG_INTR_STATUS_1, &tmp, 1);
}

/* Put sensor into low-power shutdown */
static void max30102_shutdown_best_effort(void)
{
    // MODE_CONFIG bit7 = SHDN
    (void)wr(REG_MODE_CONFIG, 0x80);
}

/* Init/configure sensor; returns:
 * - ESP_OK when initialized
 * - ESP_ERR_NOT_FOUND if missing/unplugged
 * - other errors for bus/protocol issues
 */
static esp_err_t max30102_init(void)
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

static esp_err_t max30102_read_sample(max30102_sample_t *out)
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
 *  UI update (rate-limited by task)
 * ============================= */
static float g_dc_ir = 0;
static float g_ac_amp = 0;
static float g_bpm = 0;
static bool  g_bpm_valid = false;

static void ui_update_max_async(void *arg)
{
    (void)arg;
    lv_obj_t *lbl = ui_get_hr_debug_lbl();
    if (!lbl) return;

    char buf[96];
    if (g_bpm_valid) {
        snprintf(buf, sizeof(buf),
                 "IR:%" PRIu32 " DC:%.0f AMP:%.0f\nBPM:%.0f",
                 g_ir, g_dc_ir, g_ac_amp, g_bpm);
    } else {
        snprintf(buf, sizeof(buf),
                 "IR:%" PRIu32 " DC:%.0f AMP:%.0f\nBPM: --",
                 g_ir, g_dc_ir, g_ac_amp);
    }

    bsp_display_lock(0);
    lv_label_set_text(lbl, buf);
    bsp_display_unlock();
}


/* =============================
 *  Public: Read-now trigger (UI button calls this)
 * ============================= */
void hr_request_read_now(void)
{
    if (s_hr_task) {
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
        TickType_t period_ticks = 0;
        TickType_t run_ticks = 0;

        if (g_hr_period_ms == 0 || g_hr_run_ms == 0) {
            // OFF: wait until user taps Read Now
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

            // manual run uses g_hr_run_ms if set, otherwise 10s
            run_ticks = pdMS_TO_TICKS(g_hr_run_ms ? g_hr_run_ms : (10 * 1000));
            period_ticks = pdMS_TO_TICKS(60 * 1000); // unused in this path
        } else {
            period_ticks = pdMS_TO_TICKS(g_hr_period_ms);
            run_ticks    = pdMS_TO_TICKS(g_hr_run_ms);
        }

        // ---------- IDLE: wait for next scheduled run OR read-now ----------
        if (period_ticks != 0) {
            // ---------- IDLE: wait for next scheduled run OR Read Now ----------
            TickType_t now = xTaskGetTickCount();
            TickType_t next_due = last_run_start + period_ticks;
            TickType_t wait_ticks = (next_due > now) ? (next_due - now) : 0;

            while (wait_ticks > 0) {
                TickType_t chunk = (wait_ticks > idle_poll) ? idle_poll : wait_ticks;

                // If notified => run immediately
                if (ulTaskNotifyTake(pdTRUE, chunk) > 0) {
                    ESP_LOGI(HR_TAG, "Read-now requested");
                    break;
                }

                now = xTaskGetTickCount();
                next_due = last_run_start + period_ticks;
                wait_ticks = (next_due > now) ? (next_due - now) : 0;
            }

            // Start window now (either scheduled or manual)
            last_run_start = xTaskGetTickCount();
        }

        // ---------- START WINDOW ----------
        err = max30102_init();
        if (err != ESP_OK) {
            s_max_present = false;

            uint32_t tms = esp_log_timestamp();
            if (tms - last_warn_ms > 3000) {
                last_warn_ms = tms;
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

        uint32_t ir0 = (good > 0) ? (ir_sum / good) : g_ir;
        ppg_filter_reset(&filt, (float)ir0);

        TickType_t window_start = xTaskGetTickCount();
        TickType_t next_ui = window_start; // UI update cadence

        uint32_t last_raw_log_ms = 0;
        // uint32_t next_log_ms = esp_log_timestamp();

        while ((xTaskGetTickCount() - window_start) < run_ticks) {

            max30102_sample_t s;
            err = max30102_read_sample(&s);

            if (err == ESP_OK) {
                g_red = s.red;
                g_ir  = s.ir;
                uint32_t now_ms = (uint32_t)esp_log_timestamp();

                // 1) Contact/DC sanity
                g_dc_ir = filt.dc; // will update after step, but ok

                // 2) Filter step on IR
                float ac = ppg_filter_step(&filt, (float)g_ir);

                // 3) expose for UI/quality
                g_dc_ir = filt.dc;
                g_ac_amp = filt.ac_abs_avg;

                // 4) peak detect / bpm
                // Only attempt if DC looks like contact
                bool dc_ok = (filt.dc >= (float)HR_DC_MIN && filt.dc <= (float)HR_DC_MAX);
                if (dc_ok) {
                    hr_process_sample(&hr, &filt, ac, now_ms);
                } else {
                    hr.bpm_valid = false;
                }

                // 5) accept BPM only if amplitude is decent + enough time
                bool amp_ok = (filt.ac_abs_avg >= (float)HR_AC_MIN);
                g_bpm_valid = (hr.bpm_valid && amp_ok && dc_ok);
                g_bpm = (g_bpm_valid ? hr.bpm_avg : 0);

                // 6) optional raw logging
                #if HR_LOG_RAW
                if ((now_ms - last_raw_log_ms) >= HR_LOG_RAW_EVERY_MS) {
                    last_raw_log_ms = now_ms;
                    ESP_LOGI(HR_TAG,
                    "TUNE ir=%" PRIu32 " dc=%.0f ac=%.1f amp=%.1f bpm_valid=%d bpm=%.1f",
                    g_ir,
                    filt.dc,
                    ac,
                    filt.ac_abs_avg,
                    (int)g_bpm_valid,
                    g_bpm);
                }
                
                #endif

                // UI update ~5 Hz (every 200ms)
                TickType_t t = xTaskGetTickCount();
                if (t >= next_ui) {
                    next_ui = t + pdMS_TO_TICKS(50);
                    lv_async_call(ui_update_max_async, NULL);
                }

            } else {
                // unplugged / bus issue mid-window => mark missing, exit window
                s_max_present = false;

                uint32_t tms = esp_log_timestamp();
                if (tms - last_warn_ms > 3000) {
                    last_warn_ms = tms;
                    ESP_LOGW(HR_TAG, "read failed: %s (will re-probe next time)", esp_err_to_name(err));
                }
                break;
            }

            // Internal read rate: 50 Hz (every 20ms)
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        // ---------- END WINDOW: shutdown sensor to save power ----------
        max30102_shutdown_best_effort();

        ESP_LOGI(HR_TAG, "HR window complete");
    }
}

void start_max30102_task(void)
{
    xTaskCreatePinnedToCore(max_task, "max30102", 4096, NULL, 5, NULL, 1);
}
