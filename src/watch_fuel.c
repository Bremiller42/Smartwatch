#include "watch_fuel.h"
#include "watch_i2c.h"
#include "ui_priv.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_timer.h"

#define FG_TAG "FUEL"
#define MAX17048_ADDR 0x36

#define REG_VCELL 0x02
#define REG_SOC   0x04
#define REG_MODE  0x06
#define REG_VER   0x08

// Provided by your globals
int   g_watch_batt_pct = -1;
float g_watch_batt_v   = -1.0f;

static TaskHandle_t s_fg_task = NULL;

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

static int soc_piecewise_pct(float soc)
{
    if (soc <= 80.0f) return (int)(soc + 0.5f);
    float x = 80.0f + (soc - 80.0f) * (20.0f / (94.0f - 80.0f));
    int pct = (int)(x + 0.5f);
    if (pct > 100) pct = 100;
    return pct;
}

static esp_err_t fg_probe(void)
{
    uint8_t v[2] = {0};
    return fg_rd(REG_VER, v, 2);
}

esp_err_t watch_fuel_read_vcell(float *v_out)
{
    if (!v_out) return ESP_ERR_INVALID_ARG;

    uint8_t b[2];
    esp_err_t err = fg_rd(REG_VCELL, b, 2);
    if (err != ESP_OK) return err;

    uint16_t raw = ((uint16_t)b[0] << 8) | b[1];
    *v_out = (float)raw * 0.000078125f;
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
    (void)fg_wr_u16(REG_MODE, 0x0000); // wake if asleep
    return ESP_OK;
}

static void fg_task(void *arg)
{
    (void)arg;

    while (watch_fuel_init() != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    int last_pct = -999;
    int64_t last_log_ms = 0;

    while (1) {
        const uint32_t poll_ms = g_screen_awake ? 10000 : 120000; // 10s / 2min

        float soc = -1.0f;
        float v   = -1.0f;

        esp_err_t e1 = watch_fuel_read_soc(&soc);
        esp_err_t e2 = watch_fuel_read_vcell(&v);

        if (e1 == ESP_OK && e2 == ESP_OK) {
            int pct = soc_piecewise_pct(soc);

            g_watch_batt_pct = pct;
            g_watch_batt_v   = v;

            int64_t now_ms = esp_timer_get_time() / 1000;

            // Log only on changes OR every 2 min (awake) / 10 min (asleep)
            int64_t log_period = g_screen_awake ? 120000 : 600000;
            if (pct != last_pct || (now_ms - last_log_ms) > log_period) {
                ESP_LOGI("BATT", "PCT: %d, VOLT: %.2f", pct, v);
                last_pct = pct;
                last_log_ms = now_ms;
            }

            if (g_screen_awake) {
                lv_async_call(ui_set_watch_batt_async, (void*)(intptr_t)g_watch_batt_pct);
            }
        } else {
            g_watch_batt_pct = -1;
            g_watch_batt_v   = -1.0f;
            if (g_screen_awake) {
                lv_async_call(ui_set_watch_batt_async, (void*)(intptr_t)g_watch_batt_pct);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
}

void watch_fuel_start_task(void)
{
    if (s_fg_task) return;
    xTaskCreatePinnedToCore(fg_task, "fuel", 3072, NULL, 5, &s_fg_task, 1);
}
