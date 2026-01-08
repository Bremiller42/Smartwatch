#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "freertos/FreeRTOS.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*lvgl_port_wait_cb)(void *arg);

typedef struct {
    int timer_period_ms;      // LVGL tick period
    int task_max_sleep_ms;    // max delay in lv_timer_handler loop
    int task_priority;
    int task_stack;
    int task_affinity;        // -1 no pin
} lvgl_port_cfg_t;

typedef struct {
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t    panel_handle;
    uint32_t                  buffer_size;
    uint32_t                  hres;
    uint32_t                  vres;
    uint32_t                  trans_size;
    lv_disp_rot_t             sw_rotate;
    lvgl_port_wait_cb         draw_wait_cb;
    struct {
        unsigned buff_dma   : 1;
        unsigned buff_spiram: 1;
    } flags;
} lvgl_port_display_cfg_t;

#ifdef ESP_LVGL_PORT_TOUCH_COMPONENT
#ifndef ESP_LVGL_PORT_INIT_CONFIG
#define ESP_LVGL_PORT_INIT_CONFIG() \
    (lvgl_port_cfg_t){              \
        .timer_period_ms   = 5,     \
        .task_priority     = 4,     \
        .task_stack        = 4096,  \
        .task_affinity     = -1,    \
        .task_max_sleep_ms = 50,    \
    }
#endif

typedef struct {
    lv_disp_t               *disp;
    esp_lcd_touch_handle_t   handle;
    lvgl_port_wait_cb        touch_wait_cb;
} lvgl_port_touch_cfg_t;
#endif

esp_err_t lvgl_port_init(const lvgl_port_cfg_t *cfg);
esp_err_t lvgl_port_deinit(void);

/**
 * HARD pause: stops LVGL tick + blocks LVGL task (no periodic wakeups).
 * Use when screen is off.
 */
esp_err_t lvgl_port_pause(void);

/**
 * Resume after pause.
 */
esp_err_t lvgl_port_resume(void);

/**
 * Backwards-compat (old names in your codebase)
 * stop == pause
 */
esp_err_t lvgl_port_stop(void);

lv_disp_t *lvgl_port_add_disp(const lvgl_port_display_cfg_t *disp_cfg);
esp_err_t  lvgl_port_remove_disp(lv_disp_t *disp);

#ifdef ESP_LVGL_PORT_TOUCH_COMPONENT
lv_indev_t *lvgl_port_add_touch(const lvgl_port_touch_cfg_t *touch_cfg);
esp_err_t   lvgl_port_remove_touch(lv_indev_t *touch);
#endif

bool lvgl_port_lock(uint32_t timeout_ms);
void lvgl_port_unlock(void);

void lvgl_port_flush_ready(lv_disp_t *disp);

#ifdef __cplusplus
}
#endif
