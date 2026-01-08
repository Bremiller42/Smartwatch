/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_interface.h"

#include "lv_port.h"
#include "lvgl.h"

#ifdef ESP_LVGL_PORT_TOUCH_COMPONENT
#include "esp_lcd_touch.h"
#endif

#if (ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(4, 4, 4)) || (ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(5, 0, 0))
#define LVGL_PORT_HANDLE_FLUSH_READY 0
#else
#define LVGL_PORT_HANDLE_FLUSH_READY 1
#endif

static const char *TAG = "LVGL";

/* ---------------- Types ---------------- */

typedef struct lvgl_port_ctx_s {
    SemaphoreHandle_t   lvgl_mux;
    esp_timer_handle_t  tick_timer;
    bool                running;
    int                 task_max_sleep_ms;

    // NEW: hard pause support
    TaskHandle_t        lvgl_task;
    EventGroupHandle_t  eg;
    bool                paused;
} lvgl_port_ctx_t;

#define LVGL_EG_BIT_WAKE   (1 << 0)

typedef struct {
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t    panel_handle;
    lv_disp_drv_t             disp_drv;

    uint32_t                  trans_size;
    lv_color_t               *trans_buf_1;
    lv_color_t               *trans_buf_2;
    lv_color_t               *trans_act;
    SemaphoreHandle_t         trans_done_sem;
    lv_disp_rot_t             sw_rotate;

    lvgl_port_wait_cb         draw_wait_cb;
} lvgl_port_display_ctx_t;

#ifdef ESP_LVGL_PORT_TOUCH_COMPONENT
typedef struct {
    esp_lcd_touch_handle_t  handle;
    lv_indev_drv_t          indev_drv;
    lvgl_port_wait_cb       touch_wait_cb;
} lvgl_port_touch_ctx_t;
#endif

/* ---------------- Locals ---------------- */

static lvgl_port_ctx_t lvgl_port_ctx;
static int lvgl_port_timer_period_ms = 5;

/* ---------------- Forwards ---------------- */

static void lvgl_port_task(void *arg);
static esp_err_t lvgl_port_tick_init(void);
static void lvgl_port_task_deinit(void);

#if LVGL_PORT_HANDLE_FLUSH_READY
static bool lvgl_port_flush_ready_callback(esp_lcd_panel_io_handle_t panel_io,
                                           esp_lcd_panel_io_event_data_t *edata,
                                           void *user_ctx);
#endif

static void lvgl_port_flush_callback(lv_disp_drv_t *drv,
                                     const lv_area_t *area,
                                     lv_color_t *color_map);

#ifdef ESP_LVGL_PORT_TOUCH_COMPONENT
static void lvgl_port_touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data);
#endif

/* ---------------- Public API ---------------- */

esp_err_t lvgl_port_init(const lvgl_port_cfg_t *cfg)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    ESP_GOTO_ON_FALSE(cfg->task_affinity < (configNUM_CORES), ESP_ERR_INVALID_ARG, err, TAG,
                      "Bad core number for task! Maximum core number is %d", (configNUM_CORES - 1));

    memset(&lvgl_port_ctx, 0, sizeof(lvgl_port_ctx));

    lv_init();

    lvgl_port_timer_period_ms = cfg->timer_period_ms;
    ESP_RETURN_ON_ERROR(lvgl_port_tick_init(), TAG, "");

    lvgl_port_ctx.task_max_sleep_ms = cfg->task_max_sleep_ms;
    if (lvgl_port_ctx.task_max_sleep_ms == 0) lvgl_port_ctx.task_max_sleep_ms = 500;

    lvgl_port_ctx.lvgl_mux = xSemaphoreCreateRecursiveMutex();
    ESP_GOTO_ON_FALSE(lvgl_port_ctx.lvgl_mux, ESP_ERR_NO_MEM, err, TAG, "Create LVGL mutex fail!");

    lvgl_port_ctx.eg = xEventGroupCreate();
    ESP_GOTO_ON_FALSE(lvgl_port_ctx.eg, ESP_ERR_NO_MEM, err, TAG, "Create LVGL event group fail!");

    lvgl_port_ctx.running = true;
    lvgl_port_ctx.paused = false;

    BaseType_t res;
    if (cfg->task_affinity < 0) {
        res = xTaskCreate(lvgl_port_task, "LVGL task", cfg->task_stack, NULL, cfg->task_priority, &lvgl_port_ctx.lvgl_task);
    } else {
        res = xTaskCreatePinnedToCore(lvgl_port_task, "LVGL task", cfg->task_stack, NULL, cfg->task_priority,
                                      &lvgl_port_ctx.lvgl_task, cfg->task_affinity);
    }
    ESP_GOTO_ON_FALSE(res == pdPASS, ESP_FAIL, err, TAG, "Create LVGL task fail!");

    return ESP_OK;

err:
    if (ret != ESP_OK) lvgl_port_deinit();
    return ret;
}
esp_err_t lvgl_port_pause(void)
{
    if (!lvgl_port_ctx.tick_timer || !lvgl_port_ctx.eg) return ESP_ERR_INVALID_STATE;

    esp_err_t e1 = esp_timer_stop(lvgl_port_ctx.tick_timer);
    ESP_LOGI(TAG, "pause: esp_timer_stop=%s", esp_err_to_name(e1));

    lv_timer_enable(false);
    lvgl_port_ctx.paused = true;
    xEventGroupSetBits(lvgl_port_ctx.eg, LVGL_EG_BIT_WAKE);
    return ESP_OK;
}

esp_err_t lvgl_port_resume(void)
{
    if (!lvgl_port_ctx.tick_timer || !lvgl_port_ctx.eg) return ESP_ERR_INVALID_STATE;

    lvgl_port_ctx.paused = false;
    lv_timer_enable(true);

    esp_err_t e2 = esp_timer_start_periodic(lvgl_port_ctx.tick_timer, lvgl_port_timer_period_ms * 1000);
    ESP_LOGI(TAG, "resume: esp_timer_start_periodic=%s", esp_err_to_name(e2));

    xEventGroupSetBits(lvgl_port_ctx.eg, LVGL_EG_BIT_WAKE);
    return ESP_OK;
}


esp_err_t lvgl_port_stop(void)
{
    // Backward compat: stop == pause
    return lvgl_port_pause();
}

esp_err_t lvgl_port_deinit(void)
{
    if (lvgl_port_ctx.tick_timer) {
        esp_timer_stop(lvgl_port_ctx.tick_timer);
        esp_timer_delete(lvgl_port_ctx.tick_timer);
        lvgl_port_ctx.tick_timer = NULL;
    }

    // Stop running task
    if (lvgl_port_ctx.running) {
        lvgl_port_ctx.running = false;
        if (lvgl_port_ctx.eg) xEventGroupSetBits(lvgl_port_ctx.eg, LVGL_EG_BIT_WAKE);
    } else {
        lvgl_port_task_deinit();
    }

    return ESP_OK;
}

lv_disp_t *lvgl_port_add_disp(const lvgl_port_display_cfg_t *disp_cfg)
{
    esp_err_t ret = ESP_OK;
    lv_disp_t *disp = NULL;
    lv_color_t *buf1 = NULL;
    lv_color_t *buf2 = NULL;
    lv_color_t *buf3 = NULL;
    SemaphoreHandle_t trans_done_sem = NULL;

    assert(disp_cfg && disp_cfg->io_handle && disp_cfg->panel_handle);
    assert(disp_cfg->buffer_size > 0);
    assert(disp_cfg->hres > 0 && disp_cfg->vres > 0);

    lvgl_port_display_ctx_t *disp_ctx = malloc(sizeof(lvgl_port_display_ctx_t));
    ESP_GOTO_ON_FALSE(disp_ctx, ESP_ERR_NO_MEM, err, TAG, "Not enough memory for display ctx!");
    memset(disp_ctx, 0, sizeof(*disp_ctx));

    disp_ctx->io_handle = disp_cfg->io_handle;
    disp_ctx->panel_handle = disp_cfg->panel_handle;
    disp_ctx->trans_size = disp_cfg->trans_size;
    disp_ctx->sw_rotate = disp_cfg->sw_rotate;
    disp_ctx->draw_wait_cb = disp_cfg->draw_wait_cb;

    uint32_t buff_caps = MALLOC_CAP_DEFAULT;
    if (disp_cfg->flags.buff_dma) buff_caps = MALLOC_CAP_DMA;
    else if (disp_cfg->flags.buff_spiram) buff_caps = MALLOC_CAP_SPIRAM;

    buf1 = heap_caps_malloc(disp_cfg->buffer_size * sizeof(lv_color_t), buff_caps);
    ESP_GOTO_ON_FALSE(buf1, ESP_ERR_NO_MEM, err, TAG, "No mem for LVGL buf1!");

    if (disp_ctx->trans_size) {
        uint32_t caps = MALLOC_CAP_DMA;

        buf2 = heap_caps_malloc(disp_ctx->trans_size * sizeof(lv_color_t), caps);
        ESP_GOTO_ON_FALSE(buf2, ESP_ERR_NO_MEM, err, TAG, "No mem for trans buf2!");
        disp_ctx->trans_buf_1 = buf2;

        buf3 = heap_caps_malloc(disp_ctx->trans_size * sizeof(lv_color_t), caps);
        ESP_GOTO_ON_FALSE(buf3, ESP_ERR_NO_MEM, err, TAG, "No mem for trans buf3!");
        disp_ctx->trans_buf_2 = buf3;

        trans_done_sem = xSemaphoreCreateCounting(1, 0);
        ESP_GOTO_ON_FALSE(trans_done_sem, ESP_ERR_NO_MEM, err, TAG, "No mem for trans sem!");
        disp_ctx->trans_done_sem = trans_done_sem;
    }

    lv_disp_draw_buf_t *disp_buf = malloc(sizeof(lv_disp_draw_buf_t));
    ESP_GOTO_ON_FALSE(disp_buf, ESP_ERR_NO_MEM, err, TAG, "No mem for disp buf struct!");
    lv_disp_draw_buf_init(disp_buf, buf1, NULL, disp_cfg->buffer_size);

    lv_disp_drv_init(&disp_ctx->disp_drv);
    disp_ctx->disp_drv.hor_res = disp_cfg->hres;
    disp_ctx->disp_drv.ver_res = disp_cfg->vres;
    disp_ctx->disp_drv.flush_cb = lvgl_port_flush_callback;
    disp_ctx->disp_drv.draw_buf = disp_buf;
    disp_ctx->disp_drv.user_data = disp_ctx;

    // full_refresh=1 costs bandwidth/power; you forced it. Keep for correctness,
    // but if you can disable later, it's a win.
    disp_ctx->disp_drv.full_refresh = 1;

#if LVGL_PORT_HANDLE_FLUSH_READY
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = lvgl_port_flush_ready_callback,
    };
    esp_lcd_panel_io_register_event_callbacks(disp_ctx->io_handle, &cbs, &disp_ctx->disp_drv);
#endif

    disp = lv_disp_drv_register(&disp_ctx->disp_drv);
    return disp;

err:
    if (buf1) free(buf1);
    if (buf2) free(buf2);
    if (buf3) free(buf3);
    if (trans_done_sem) vSemaphoreDelete(trans_done_sem);
    if (disp_ctx) free(disp_ctx);
    (void)ret;
    return NULL;
}

esp_err_t lvgl_port_remove_disp(lv_disp_t *disp)
{
    assert(disp && disp->driver);
    lv_disp_drv_t *disp_drv = disp->driver;
    lvgl_port_display_ctx_t *disp_ctx = (lvgl_port_display_ctx_t *)disp_drv->user_data;

    lv_disp_remove(disp);

    if (disp_drv->draw_buf) {
        if (disp_drv->draw_buf->buf1) { free(disp_drv->draw_buf->buf1); disp_drv->draw_buf->buf1 = NULL; }
        if (disp_drv->draw_buf->buf2) { free(disp_drv->draw_buf->buf2); disp_drv->draw_buf->buf2 = NULL; }
        free(disp_drv->draw_buf);
        disp_drv->draw_buf = NULL;
    }

    free(disp_ctx);
    return ESP_OK;
}

#ifdef ESP_LVGL_PORT_TOUCH_COMPONENT
lv_indev_t *lvgl_port_add_touch(const lvgl_port_touch_cfg_t *touch_cfg)
{
    assert(touch_cfg && touch_cfg->disp && touch_cfg->handle);

    lvgl_port_touch_ctx_t *touch_ctx = malloc(sizeof(lvgl_port_touch_ctx_t));
    if (!touch_ctx) {
        ESP_LOGE(TAG, "Not enough memory for touch context allocation!");
        return NULL;
    }
    memset(touch_ctx, 0, sizeof(*touch_ctx));

    touch_ctx->handle = touch_cfg->handle;
    touch_ctx->touch_wait_cb = touch_cfg->touch_wait_cb;

    lv_indev_drv_init(&touch_ctx->indev_drv);
    touch_ctx->indev_drv.type = LV_INDEV_TYPE_POINTER;
    touch_ctx->indev_drv.disp = touch_cfg->disp;
    touch_ctx->indev_drv.read_cb = lvgl_port_touchpad_read;
    touch_ctx->indev_drv.user_data = touch_ctx;

    // Your tuning retained
    touch_ctx->indev_drv.scroll_limit = 20;
    touch_ctx->indev_drv.scroll_throw = 30;
    touch_ctx->indev_drv.long_press_time = 300;

    return lv_indev_drv_register(&touch_ctx->indev_drv);
}

esp_err_t lvgl_port_remove_touch(lv_indev_t *touch)
{
    assert(touch);
    lv_indev_drv_t *indev_drv = touch->driver;
    assert(indev_drv);
    lvgl_port_touch_ctx_t *touch_ctx = (lvgl_port_touch_ctx_t *)indev_drv->user_data;

    lv_indev_delete(touch);
    if (touch_ctx) free(touch_ctx);
    return ESP_OK;
}
#endif

bool lvgl_port_lock(uint32_t timeout_ms)
{
    assert(lvgl_port_ctx.lvgl_mux && "lvgl_port_init must be called first");
    const TickType_t timeout_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(lvgl_port_ctx.lvgl_mux, timeout_ticks) == pdTRUE;
}

void lvgl_port_unlock(void)
{
    assert(lvgl_port_ctx.lvgl_mux && "lvgl_port_init must be called first");
    xSemaphoreGiveRecursive(lvgl_port_ctx.lvgl_mux);
}

void lvgl_port_flush_ready(lv_disp_t *disp)
{
    assert(disp && disp->driver);
    lv_disp_flush_ready(disp->driver);
}

/* ---------------- Private ---------------- */

static void lvgl_port_task(void *arg)
{
    (void)arg;
    uint32_t task_delay_ms = lvgl_port_ctx.task_max_sleep_ms;

    ESP_LOGI(TAG, "Starting LVGL task");

    while (lvgl_port_ctx.running) {

        // HARD PAUSE: block indefinitely until resume() sets WAKE bit
        if (lvgl_port_ctx.paused) {
            xEventGroupWaitBits(lvgl_port_ctx.eg, LVGL_EG_BIT_WAKE, pdTRUE, pdFALSE, portMAX_DELAY);
            continue;
        }

        if (lvgl_port_lock(0)) {
            task_delay_ms = lv_timer_handler();
            lvgl_port_unlock();
        }

        if ((task_delay_ms > (uint32_t)lvgl_port_ctx.task_max_sleep_ms) || (task_delay_ms == 1)) {
            task_delay_ms = lvgl_port_ctx.task_max_sleep_ms;
        } else if (task_delay_ms < 1) {
            task_delay_ms = 1;
        }

        // Also allow immediate wake (e.g., resume) without waiting full delay
        (void)xEventGroupWaitBits(lvgl_port_ctx.eg, LVGL_EG_BIT_WAKE, pdTRUE, pdFALSE, pdMS_TO_TICKS(task_delay_ms));
    }

    lvgl_port_task_deinit();
    vTaskDelete(NULL);
}

static void lvgl_port_task_deinit(void)
{
    if (lvgl_port_ctx.lvgl_mux) vSemaphoreDelete(lvgl_port_ctx.lvgl_mux);
    if (lvgl_port_ctx.eg) vEventGroupDelete(lvgl_port_ctx.eg);
    memset(&lvgl_port_ctx, 0, sizeof(lvgl_port_ctx));

#if LV_ENABLE_GC || !LV_MEM_CUSTOM
    lv_deinit();
#endif
}

#if LVGL_PORT_HANDLE_FLUSH_READY
static bool lvgl_port_flush_ready_callback(esp_lcd_panel_io_handle_t panel_io,
                                           esp_lcd_panel_io_event_data_t *edata,
                                           void *user_ctx)
{
    (void)panel_io;
    (void)edata;

    BaseType_t taskAwake = pdFALSE;

    lv_disp_drv_t *disp_drv = (lv_disp_drv_t *)user_ctx;
    assert(disp_drv);
    lvgl_port_display_ctx_t *disp_ctx = disp_drv->user_data;
    assert(disp_ctx);

    if (disp_ctx->trans_done_sem) {
        xSemaphoreGiveFromISR(disp_ctx->trans_done_sem, &taskAwake);
        if (taskAwake) portYIELD_FROM_ISR();
    }

    return false;
}
#endif

static void lvgl_port_flush_callback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    assert(drv && area && color_map);
    lvgl_port_display_ctx_t *disp_ctx = (lvgl_port_display_ctx_t *)drv->user_data;
    assert(disp_ctx);

    // If paused, do NOT push frames (screen is off anyway)
    if (lvgl_port_ctx.paused) {
        lv_disp_flush_ready(drv);
        return;
    }

    const int x_start = area->x1;
    const int x_end   = area->x2;
    const int y_start = area->y1;
    const int y_end   = area->y2;

    const int width  = x_end - x_start + 1;
    const int height = y_end - y_start + 1;

    lv_color_t *from = color_map;
    lv_color_t *to   = NULL;

    if (disp_ctx->trans_size) {

        int x_draw_start = 0, x_draw_end = 0;
        int y_draw_start = 0, y_draw_end = 0;
        int trans_count = 0;

        disp_ctx->trans_act = disp_ctx->trans_buf_1;
        int rotate = disp_ctx->sw_rotate;

        int x_start_tmp = 0, x_end_tmp = 0, max_width = 0, trans_width = 0;
        int y_start_tmp = 0, y_end_tmp = 0, max_height = 0, trans_height = 0;

        if (LV_DISP_ROT_270 == rotate || LV_DISP_ROT_90 == rotate) {
            max_width = ((disp_ctx->trans_size / height) > (uint32_t)width) ? width : (int)(disp_ctx->trans_size / height);
            trans_count = width / max_width + (width % max_width ? 1 : 0);
            x_start_tmp = x_start;
            x_end_tmp   = x_end;
        } else {
            max_height = ((disp_ctx->trans_size / width) > (uint32_t)height) ? height : (int)(disp_ctx->trans_size / width);
            trans_count = height / max_height + (height % max_height ? 1 : 0);
            y_start_tmp = y_start;
            y_end_tmp   = y_end;
        }

        for (int i = 0; i < trans_count; i++) {

            if (LV_DISP_ROT_90 == rotate) {
                trans_width = (x_end - x_start_tmp + 1) > max_width ? max_width : (x_end - x_start_tmp + 1);
                x_end_tmp = (x_end - x_start_tmp + 1) > max_width ? (x_start_tmp + max_width - 1) : x_end;
            } else if (LV_DISP_ROT_270 == rotate) {
                trans_width = (x_end_tmp - x_start + 1) > max_width ? max_width : (x_end_tmp - x_start + 1);
                x_start_tmp = (x_end_tmp - x_start + 1) > max_width ? (x_end_tmp - trans_width + 1) : x_start;
            } else if (LV_DISP_ROT_NONE == rotate) {
                trans_height = (y_end - y_start_tmp + 1) > max_height ? max_height : (y_end - y_start_tmp + 1);
                y_end_tmp = (y_end - y_start_tmp + 1) > max_height ? (y_start_tmp + max_height - 1) : y_end;
            } else {
                trans_height = (y_end_tmp - y_start + 1) > max_height ? max_height : (y_end_tmp - y_start + 1);
                y_start_tmp = (y_end_tmp - y_start + 1) > max_height ? (y_end_tmp - max_height + 1) : y_start;
            }

            disp_ctx->trans_act = (disp_ctx->trans_act == disp_ctx->trans_buf_1) ? disp_ctx->trans_buf_2 : disp_ctx->trans_buf_1;
            to = disp_ctx->trans_act;

            switch (rotate) {
                case LV_DISP_ROT_90:
                    for (int y = 0; y < height; y++) {
                        for (int x = 0; x < trans_width; x++) {
                            *(to + x * height + (height - y - 1)) = *(from + y * width + x_start_tmp + x);
                        }
                    }
                    x_draw_start = drv->ver_res - y_end - 1;
                    x_draw_end   = drv->ver_res - y_start - 1;
                    y_draw_start = x_start_tmp;
                    y_draw_end   = x_end_tmp;
                    break;

                case LV_DISP_ROT_270:
                    for (int y = 0; y < height; y++) {
                        for (int x = 0; x < trans_width; x++) {
                            *(to + (trans_width - x - 1) * height + y) = *(from + y * width + x_start_tmp + x);
                        }
                    }
                    x_draw_start = y_start;
                    x_draw_end   = y_end;
                    y_draw_start = drv->hor_res - x_end_tmp - 1;
                    y_draw_end   = drv->hor_res - x_start_tmp - 1;
                    break;

                case LV_DISP_ROT_180:
                    for (int y = 0; y < trans_height; y++) {
                        for (int x = 0; x < width; x++) {
                            *(to + (trans_height - y - 1) * width + (width - x - 1)) =
                                *(from + y_start_tmp * width + y * width + x);
                        }
                    }
                    x_draw_start = drv->hor_res - x_end - 1;
                    x_draw_end   = drv->hor_res - x_start - 1;
                    y_draw_start = drv->ver_res - y_end_tmp - 1;
                    y_draw_end   = drv->ver_res - y_start_tmp - 1;
                    break;

                case LV_DISP_ROT_NONE:
                default:
                    for (int y = 0; y < trans_height; y++) {
                        for (int x = 0; x < width; x++) {
                            *(to + y * width + x) = *(from + y_start_tmp * width + y * width + x);
                        }
                    }
                    x_draw_start = x_start;
                    x_draw_end   = x_end;
                    y_draw_start = y_start_tmp;
                    y_draw_end   = y_end_tmp;
                    break;
            }

            if (i == 0) {
                if (disp_ctx->draw_wait_cb) disp_ctx->draw_wait_cb(disp_ctx->panel_handle->user_data);
                xSemaphoreGive(disp_ctx->trans_done_sem);
            }

            xSemaphoreTake(disp_ctx->trans_done_sem, portMAX_DELAY);
            esp_lcd_panel_draw_bitmap(disp_ctx->panel_handle,
                                      x_draw_start, y_draw_start,
                                      x_draw_end + 1, y_draw_end + 1, to);

            if (LV_DISP_ROT_90 == rotate) x_start_tmp += max_width;
            else if (LV_DISP_ROT_270 == rotate) x_end_tmp -= max_width;
            else if (LV_DISP_ROT_NONE == rotate) y_start_tmp += max_height;
            else y_end_tmp -= max_height;
        }
    } else {
        esp_lcd_panel_draw_bitmap(disp_ctx->panel_handle, x_start, y_start, x_end + 1, y_end + 1, color_map);
    }

    lv_disp_flush_ready(drv);
}

#ifdef ESP_LVGL_PORT_TOUCH_COMPONENT
static void lvgl_port_touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    assert(indev_drv && data);
    lvgl_port_touch_ctx_t *touch_ctx = (lvgl_port_touch_ctx_t *)indev_drv->user_data;
    assert(touch_ctx && touch_ctx->handle);

    // If LVGL is paused, don't hit I2C at all (saves power + prevents bogus reads)
    if (lvgl_port_ctx.paused) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    uint16_t touchpad_x[1] = {0};
    uint16_t touchpad_y[1] = {0};
    uint8_t  touchpad_cnt  = 0;

    bool touch_int = true;
    if (touch_ctx->touch_wait_cb) {
        touch_int = touch_ctx->touch_wait_cb(touch_ctx->handle->config.user_data);
    }

    if (touch_int) {
        esp_lcd_touch_read_data(touch_ctx->handle);
        bool pressed = esp_lcd_touch_get_coordinates(touch_ctx->handle, touchpad_x, touchpad_y, NULL, &touchpad_cnt, 1);

        if (pressed && touchpad_cnt > 0) {
            data->point.x = touchpad_x[0];
            data->point.y = touchpad_y[0];
            data->state = LV_INDEV_STATE_PRESSED;
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
#endif

static void lvgl_port_tick_increment(void *arg)
{
    (void)arg;
    lv_tick_inc(lvgl_port_timer_period_ms);
}

static esp_err_t lvgl_port_tick_init(void)
{
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &lvgl_port_tick_increment,
        .name = "LVGL tick",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&lvgl_tick_timer_args, &lvgl_port_ctx.tick_timer),
                        TAG, "Creating LVGL timer failed!");
    return esp_timer_start_periodic(lvgl_port_ctx.tick_timer, lvgl_port_timer_period_ms * 1000);
}
