#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h> // for unlink()

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "watch_sdcard.h"

static const char *TAG = "SDCARD";

#define PIN_NUM_MISO  13
#define PIN_NUM_MOSI  11
#define PIN_NUM_CLK   12
#define PIN_NUM_CS    10

#define SDCARD_SPI_HOST  SPI3_HOST

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;
static bool s_bus_inited = false;
static SemaphoreHandle_t s_sd_mutex = NULL;

static void sdcard_gpio_preinit(void)
{
    gpio_reset_pin(PIN_NUM_CS);
    gpio_set_direction(PIN_NUM_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_NUM_CS, 1);
    gpio_set_pull_mode(PIN_NUM_CS, GPIO_PULLUP_ONLY);
}

esp_err_t watch_sdcard_mount(void)
{
    if (s_mounted) return ESP_OK;

    sdcard_gpio_preinit();

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16 * 1024,   // fine with DMA
        .intr_flags = 0,                // don’t demand IRAM interrupts
    };

    esp_err_t err = spi_bus_initialize(SDCARD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }
    s_bus_inited = true;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SDCARD_SPI_HOST;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT; // 20MHz; if flaky, try SDMMC_FREQ_PROBING then bump later

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = SDCARD_SPI_HOST;

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(TAG, "Mounting SD card at /sdcard ...");
    err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_fat_sdspi_mount failed: %s", esp_err_to_name(err));
        if (s_bus_inited) {
            spi_bus_free(SDCARD_SPI_HOST);
            s_bus_inited = false;
        }
        return err;
    }

    s_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

void watch_sdcard_unmount(void)
{
    if (!s_mounted) return;

    esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
    s_card = NULL;
    s_mounted = false;

    // This matters: frees interrupts allocated by the SPI host + device.
    if (s_bus_inited) {
        spi_bus_free(SDCARD_SPI_HOST);
        s_bus_inited = false;
    }

    ESP_LOGI(TAG, "SD card unmounted");
}


static void sd_helpers_init_once(void)
{
    if (s_sd_mutex) return;
    s_sd_mutex = xSemaphoreCreateMutex();
    // If allocation fails, we’ll handle it later by returning ESP_ERR_NO_MEM.
}

static esp_err_t take_lock(uint32_t timeout_ms)
{
    sd_helpers_init_once();
    if (!s_sd_mutex) return ESP_ERR_NO_MEM;

    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_sd_mutex, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void give_lock(void)
{
    if (s_sd_mutex) {
        xSemaphoreGive(s_sd_mutex);
    }
}

esp_err_t watch_sdcard_do(watch_sd_work_fn_t work, void *ctx, uint32_t timeout_ms)
{
    if (!work) return ESP_ERR_INVALID_ARG;

    esp_err_t e = take_lock(timeout_ms);
    if (e != ESP_OK) return e;

    // From here, only one task is allowed to do SD work at a time.
    esp_err_t mount_e = watch_sdcard_mount();
    if (mount_e != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(mount_e));
        give_lock();
        return mount_e;
    }

    esp_err_t work_e = work(ctx);

    // Always unmount after callback to free interrupts/resources.
    watch_sdcard_unmount();

    give_lock();
    return work_e;
}

/* ---------------- Convenience: write file ---------------- */

typedef struct {
    const char *path;
    const void *data;
    size_t len;
    bool append;
} write_ctx_t;

static esp_err_t write_work(void *p)
{
    write_ctx_t *w = (write_ctx_t*)p;
    if (!w || !w->path || (!w->data && w->len)) return ESP_ERR_INVALID_ARG;

    const char *mode = w->append ? "ab" : "wb";
    FILE *f = fopen(w->path, mode);
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed", w->path);
        return ESP_FAIL;
    }

    size_t n = fwrite(w->data, 1, w->len, f);
    fclose(f);

    if (n != w->len) {
        ESP_LOGE(TAG, "fwrite short: %u/%u", (unsigned)n, (unsigned)w->len);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t watch_sdcard_write_file(const char *path, const void *data, size_t len, bool append, uint32_t timeout_ms)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    write_ctx_t ctx = {
        .path = path,
        .data = data,
        .len = len,
        .append = append,
    };
    return watch_sdcard_do(write_work, &ctx, timeout_ms);
}

/* ---------------- Convenience: append line ---------------- */

typedef struct {
    const char *path;
    const char *line;
} line_ctx_t;

static esp_err_t append_line_work(void *p)
{
    line_ctx_t *c = (line_ctx_t*)p;
    if (!c || !c->path || !c->line) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(c->path, "ab");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed", c->path);
        return ESP_FAIL;
    }

    size_t L = strlen(c->line);
    if (L > 0) {
        if (fwrite(c->line, 1, L, f) != L) {
            fclose(f);
            return ESP_FAIL;
        }
        if (c->line[L - 1] != '\n') {
            const char nl = '\n';
            if (fwrite(&nl, 1, 1, f) != 1) {
                fclose(f);
                return ESP_FAIL;
            }
        }
    } else {
        // empty line -> just newline
        const char nl = '\n';
        if (fwrite(&nl, 1, 1, f) != 1) {
            fclose(f);
            return ESP_FAIL;
        }
    }

    fclose(f);
    return ESP_OK;
}

esp_err_t watch_sdcard_append_line(const char *path, const char *line, uint32_t timeout_ms)
{
    if (!path || !line) return ESP_ERR_INVALID_ARG;
    line_ctx_t ctx = { .path = path, .line = line };
    return watch_sdcard_do(append_line_work, &ctx, timeout_ms);
}

/* ---------------- Convenience: read entire file ---------------- */

typedef struct {
    const char *path;
    uint8_t **out_buf;
    size_t *out_len;
    size_t max_bytes;
} read_ctx_t;

static esp_err_t read_work(void *p)
{
    read_ctx_t *r = (read_ctx_t*)p;
    if (!r || !r->path || !r->out_buf || !r->out_len) return ESP_ERR_INVALID_ARG;

    *r->out_buf = NULL;
    *r->out_len = 0;

    FILE *f = fopen(r->path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed", r->path);
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return ESP_FAIL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }

    size_t size = (size_t)sz;
    if (r->max_bytes && size > r->max_bytes) {
        fclose(f);
        ESP_LOGE(TAG, "file too big (%u > %u)", (unsigned)size, (unsigned)r->max_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = (uint8_t*)malloc(size ? size : 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t n = fread(buf, 1, size, f);
    fclose(f);

    if (n != size) {
        free(buf);
        ESP_LOGE(TAG, "fread short: %u/%u", (unsigned)n, (unsigned)size);
        return ESP_FAIL;
    }

    *r->out_buf = buf;
    *r->out_len = size;
    return ESP_OK;
}

esp_err_t watch_sdcard_read_file(const char *path, uint8_t **out_buf, size_t *out_len, size_t max_bytes, uint32_t timeout_ms)
{
    if (!path || !out_buf || !out_len) return ESP_ERR_INVALID_ARG;
    read_ctx_t ctx = {
        .path = path,
        .out_buf = out_buf,
        .out_len = out_len,
        .max_bytes = max_bytes,
    };
    return watch_sdcard_do(read_work, &ctx, timeout_ms);
}


static esp_err_t selftest_work(void *ctx)
{
    (void)ctx;
    const char *test_path = "/sdcard/sd_test.txt";
    const char *test_data = "SDCARD_TEST_OK_123456789";

    ESP_LOGI("SD_TEST", "Writing test file...");

    FILE *f = fopen(test_path, "wb");
    if (!f) {
        ESP_LOGE("SD_TEST", "Failed to open test file for write");
        return ESP_FAIL;
    }
    fwrite(test_data, 1, strlen(test_data), f);
    fclose(f);

    ESP_LOGI("SD_TEST", "Reading test file...");

    f = fopen(test_path, "rb");
    if (!f) {
        ESP_LOGE("SD_TEST", "Failed to open test file for read");
        return ESP_FAIL;
    }

    char buf[64] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);

    if (n != strlen(test_data) || memcmp(buf, test_data, strlen(test_data)) != 0) {
        ESP_LOGE("SD_TEST", "Data mismatch!");
        ESP_LOGE("SD_TEST", "Read: '%s'", buf);
        return ESP_FAIL;
    }

    ESP_LOGI("SD_TEST", "Deleting test file...");
    if (unlink(test_path) != 0) {
        ESP_LOGW("SD_TEST", "unlink failed (non-fatal)");
    }

    ESP_LOGI("SD_TEST", "SD CARD SELF-TEST OK");
    return ESP_OK;
}

esp_err_t watch_sdcard_self_test(uint32_t timeout_ms)
{
    return watch_sdcard_do(selftest_work, NULL, timeout_ms);
}