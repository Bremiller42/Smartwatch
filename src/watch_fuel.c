#include "watch_fuel.h"
#include "watch_i2c.h"
#include "watch_ui.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_timer.h"

#define FG_TAG "FUEL"
#define MAX17048_ADDR 0x36

// Registers (datasheet)
#define REG_VCELL 0x02
#define REG_SOC   0x04
#define REG_MODE  0x06
#define REG_VER   0x08

int   g_watch_batt_pct = -1;
float g_watch_batt_v   = -1.0f;

static TaskHandle_t s_fg_task = NULL;

// --- I2C helpers (match your MAX30102 style) ---
static esp_err_t fg_rd(uint8_t reg, uint8_t *data, size_t len)
{
    return watch_i2c_write_read(MAX17048_ADDR, &reg, 1, data, len, pdMS_TO_TICKS(50));

}

static esp_err_t fg_wr_u16(uint8_t reg, uint16_t val_be)
{
    uint8_t buf[3];
    buf[0] = reg;
    buf[1] = (uint8_t)(val_be >> 8);
    buf[2] = (uint8_t)(val_be & 0xFF);
    return watch_i2c_write(MAX17048_ADDR, buf, sizeof(buf), pdMS_TO_TICKS(50));

}

static esp_err_t fg_probe(void)
{
    uint8_t v[2] = {0};
    // VERSION is a safe read; if we get ACK + 2 bytes, it's there
    return fg_rd(REG_VER, v, 2);
}

// Datasheet / common libs: MAX17048 VCELL is 16-bit with 78.125uV per LSB (full-scale 5.12V).
// SOC: MSB integer %, LSB fractional (1/256 %).
esp_err_t watch_fuel_read_vcell(float *v_out)
{
    if (!v_out) return ESP_ERR_INVALID_ARG;

    uint8_t b[2];
    esp_err_t err = fg_rd(REG_VCELL, b, 2);
    if (err != ESP_OK) return err;

    uint16_t raw = ((uint16_t)b[0] << 8) | b[1];
    *v_out = (float)raw * 0.000078125f; // 78.125 uV
    return ESP_OK;
}

esp_err_t watch_fuel_read_soc(float *pct_out)
{
    if (!pct_out) return ESP_ERR_INVALID_ARG;

    uint8_t b[2];
    esp_err_t err = fg_rd(REG_SOC, b, 2);
    if (err != ESP_OK) return err;

    uint16_t raw = ((uint16_t)b[0] << 8) | b[1];
    uint8_t  ip  = (raw >> 8) & 0xFF;
    uint8_t  fp  = raw & 0xFF;

    *pct_out = (float)ip + ((float)fp / 256.0f);
    return ESP_OK;
}

esp_err_t watch_fuel_init(void)
{
    esp_err_t err = watch_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGE(FG_TAG, "I2C init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = fg_probe();
    if (err != ESP_OK) {
        ESP_LOGW(FG_TAG, "MAX17048 not found at 0x%02X (%s)", MAX17048_ADDR, esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(FG_TAG, "MAX17048 present at 0x%02X", MAX17048_ADDR);

    // Optional: ensure not in sleep. MODE register bit7 = SLEEP (datasheet).
    // Clear sleep (write 0x0000) is a safe “wake” pattern used in many drivers.
    (void)fg_wr_u16(REG_MODE, 0x0000);

    return ESP_OK;
}
// ---- UI hook (async-safe) ----
// Async trampoline (LVGL thread context)

static void fg_task(void *arg)
{
    (void)arg;

    // Try init until MAX17048 is present
    while (watch_fuel_init() != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    const uint32_t poll_ms = 2000;  // <-- define your polling interval here

    int last_pct = -999;
    int64_t last_log_ms = 0;

    while (1) {
        float soc = -1.0f;
        float v   = -1.0f;

        esp_err_t e1 = watch_fuel_read_soc(&soc);
        esp_err_t e2 = watch_fuel_read_vcell(&v);

        if (e1 == ESP_OK && e2 == ESP_OK) {
            int pct = (int)(soc + 0.5f);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;

            g_watch_batt_pct = pct;
            g_watch_batt_v   = v;

            int64_t now_ms = esp_timer_get_time() / 1000;

            if (pct != last_pct || (now_ms - last_log_ms) > 60000) {
                ESP_LOGI("BATT", "PCT: %d, VOLT: %.2f", pct, v);
                last_pct = pct;
                last_log_ms = now_ms;
            }

            // Update LVGL safely
            lv_async_call(ui_set_watch_batt_async, (void*)(intptr_t)g_watch_batt_pct);
        } else {
            g_watch_batt_pct = -1;
            g_watch_batt_v   = -1.0f;
            lv_async_call(ui_set_watch_batt_async, (void*)(intptr_t)g_watch_batt_pct);
        }

        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
}

void watch_fuel_start_task(void)
{
    if (s_fg_task) return; // already running
    xTaskCreatePinnedToCore(fg_task, "fuel", 3072, NULL, 5, NULL, 1);
}
