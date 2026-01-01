#include "watch_i2c.h"

#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

static const char *TAG = "i2c1";
static esp_err_t i2c_probe_addr(i2c_port_t port, uint8_t addr_7bit, TickType_t timeout)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;

    i2c_master_start(cmd);
    // Send address + write bit; expect ACK if device exists
    i2c_master_write_byte(cmd, (addr_7bit << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(port, cmd, timeout);
    i2c_cmd_link_delete(cmd);
    return ret;
}

static void i2c_scan(void)
{
    ESP_LOGI(TAG, "Scanning I2C port %d...", (int)WATCH_I2C_PORT);

    int found = 0;
    for (int addr = 1; addr < 127; addr++) {
        if (i2c_probe_addr(WATCH_I2C_PORT, (uint8_t)addr, pdMS_TO_TICKS(20)) == ESP_OK) {
            ESP_LOGI(TAG, "Found device at 0x%02X", addr);
            found++;
        }
    }
    ESP_LOGI(TAG, "Scan done. Found %d device(s).", found);
}

esp_err_t watch_i2c_init(void)
{
    static bool inited = false;
    if (inited) return ESP_OK;

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = WATCH_I2C_SDA_GPIO,
        .scl_io_num = WATCH_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = WATCH_I2C_FREQ_HZ,
        .clk_flags = 0
    };

    ESP_RETURN_ON_ERROR(i2c_param_config(WATCH_I2C_PORT, &conf), TAG, "i2c_param_config failed");

    esp_err_t err = i2c_driver_install(WATCH_I2C_PORT, conf.mode, 0, 0, 0);
    if (err == ESP_ERR_INVALID_STATE) {
        // already installed (fine) — still scan
        ESP_LOGW(TAG, "I2C port %d driver already installed; reusing", (int)WATCH_I2C_PORT);
        inited = true;
        i2c_scan();
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "i2c_driver_install failed");

    inited = true;
    ESP_LOGI(TAG, "I2C init OK on port %d", (int)WATCH_I2C_PORT);

    i2c_scan();
    return ESP_OK;
}
