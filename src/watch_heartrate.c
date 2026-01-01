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
uint32_t g_hr_run_ms    = 10 * 1000;   // default: 10 seconds
// How often to run a cycle (ms)
uint32_t g_hr_period_ms = 60 * 1000;   // default: every 60 seconds

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
    return i2c_master_write_to_device(WATCH_I2C_PORT, MAX30102_ADDR, buf, sizeof(buf), pdMS_TO_TICKS(50));
}

static esp_err_t rd(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(WATCH_I2C_PORT, MAX30102_ADDR, &reg, 1, data, len, pdMS_TO_TICKS(50));
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
    err = wr(REG_FIFO_CONFIG, 0x00);
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

/* =============================
 *  UI update (rate-limited by task)
 * ============================= */
static void ui_update_max_async(void *arg)
{
    (void)arg;
    lv_obj_t *lbl = ui_get_hr_debug_lbl();
    if (!lbl) return;

    char buf[64];
    snprintf(buf, sizeof(buf), "RED:%" PRIu32 " IR:%" PRIu32, g_red, g_ir);

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
        vTaskDelete(NULL);
        return;
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

        TickType_t window_start = xTaskGetTickCount();
        TickType_t next_ui = window_start; // UI update cadence

        // sample loop
        while ((xTaskGetTickCount() - window_start) < run_ticks) {

            max30102_sample_t s;
            err = max30102_read_sample(&s);

            if (err == ESP_OK) {
                g_red = s.red;
                g_ir  = s.ir;

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
