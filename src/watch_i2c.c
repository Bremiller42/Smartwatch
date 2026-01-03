#include "watch_i2c.h"

#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

static const char *TAG = "i2c1";
static SemaphoreHandle_t s_i2c_mutex = NULL;
static SemaphoreHandle_t s_init_mux  = NULL;
static bool inited = false;

static esp_err_t i2c_lock(TickType_t to)
{
    if (!s_i2c_mutex) return ESP_ERR_INVALID_STATE;
    return (xSemaphoreTake(s_i2c_mutex, to) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}
static void i2c_unlock(void)
{
    if (s_i2c_mutex) xSemaphoreGive(s_i2c_mutex);
}

esp_err_t watch_i2c_init(void)
{
    esp_err_t err = ESP_OK;

    // Create mutexes once
    if (!s_i2c_mutex) {
        s_i2c_mutex = xSemaphoreCreateMutex();
        if (!s_i2c_mutex) return ESP_ERR_NO_MEM;
    }
    if (!s_init_mux) {
        s_init_mux = xSemaphoreCreateMutex();
        if (!s_init_mux) return ESP_ERR_NO_MEM;
    }

    if (inited) return ESP_OK;

    if (xSemaphoreTake(s_init_mux, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (inited) {
        xSemaphoreGive(s_init_mux);
        return ESP_OK;
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = WATCH_I2C_SDA_GPIO,
        .scl_io_num = WATCH_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = WATCH_I2C_FREQ_HZ,
        .clk_flags = 0
    };

    err = i2c_param_config(WATCH_I2C_PORT, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(err));
        goto out;
    }

    err = i2c_driver_install(WATCH_I2C_PORT, conf.mode, 0, 0, 0);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "I2C port %d already installed; reusing", (int)WATCH_I2C_PORT);
        err = ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(err));
        goto out;
    }

    inited = true;
    ESP_LOGI(TAG, "I2C init OK on port %d", (int)WATCH_I2C_PORT);

out:
    xSemaphoreGive(s_init_mux);
    return err;
}

esp_err_t watch_i2c_write_read(uint8_t addr7, const uint8_t *w, size_t wl,
                               uint8_t *r, size_t rl, TickType_t to)
{
    esp_err_t err = i2c_lock(to);
    if (err != ESP_OK) return err;

    err = i2c_master_write_read_device(WATCH_I2C_PORT, addr7, w, wl, r, rl, to);

    i2c_unlock();
    return err;
}

esp_err_t watch_i2c_write(uint8_t addr7, const uint8_t *w, size_t wl, TickType_t to)
{
    esp_err_t err = i2c_lock(to);
    if (err != ESP_OK) return err;

    err = i2c_master_write_to_device(WATCH_I2C_PORT, addr7, w, wl, to);

    i2c_unlock();
    return err;
}
