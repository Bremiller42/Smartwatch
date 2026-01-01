// watch_i2c.h
#pragma once
#include "driver/i2c.h"
#include "esp_err.h"

#define WATCH_I2C_PORT      I2C_NUM_1
#define WATCH_I2C_SDA_GPIO  18    // <-- set to your pins
#define WATCH_I2C_SCL_GPIO  17  
#define WATCH_I2C_FREQ_HZ   400000

esp_err_t watch_i2c_init(void);
