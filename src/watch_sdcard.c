// FILE: watch_sdcard.c

#include "watch_sdcard.h"

#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "ff.h"   
#include "ui_priv.h"

#include "watch_screen_timeout.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>   // unlink()
#include <stdio.h>

static const char *TAG = "SDCARD";

#define PIN_NUM_MISO  13
#define PIN_NUM_MOSI  11
#define PIN_NUM_CLK   12
#define PIN_NUM_CS    10

#define SDCARD_SPI_HOST     SPI3_HOST
#define SDCARD_MOUNT_POINT  "/sdcard"

/* ---------------- State ---------------- */
static sdmmc_card_t *s_card       = NULL;
static bool          s_mounted    = false;
static bool          s_bus_inited = false;
static bool          s_bus_owned  = false;

static SemaphoreHandle_t s_sd_mutex = NULL;


typedef enum {
    SD_JOB_WORK = 0,
    SD_JOB_UNMOUNT,
} sd_job_type_t;

typedef struct {
    sd_job_type_t      type;        // NEW
    watch_sd_work_fn_t fn;
    void              *ctx;
    uint32_t           timeout_ms;

    // completion
    SemaphoreHandle_t  done_sem;
    esp_err_t          result;
} sd_job_t;


/* ---------------- Cached SD “Label” ----------------
 * NOTE: This is NOT the FAT volume label (since your build lacks f_getlabel()).
 * We cache a stable identifier from CID name after mount succeeds.
 * UI can show "/sdcard (XXXXX)" and never touches SD/FatFs.
 */
static char s_sd_id_cached[32] = "(unmounted)";
static uint64_t s_sd_total_kb = 0;
static uint64_t s_sd_free_kb  = 0;

uint64_t watch_sdcard_total_kb_cached(void) { return s_sd_total_kb; }
uint64_t watch_sdcard_free_kb_cached(void)  { return s_sd_free_kb;  }

const char *watch_sdcard_id_cached(void)
{
    return s_sd_id_cached;
}

static inline void safe_strlcpy(char *dst, const char *src, size_t dst_sz)
{
    if (!dst || dst_sz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= dst_sz) n = dst_sz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void sd_id_set_unmounted(void)
{
    safe_strlcpy(s_sd_id_cached, "(unmounted)", sizeof(s_sd_id_cached));
}

static void sd_id_set_unknown(void)
{
    safe_strlcpy(s_sd_id_cached, "(unknown)", sizeof(s_sd_id_cached));
}

static void sd_space_set_unmounted(void)
{
    s_sd_total_kb = 0;
    s_sd_free_kb  = 0;
}

static void sd_space_update_after_mount(void)
{
    FATFS *fs = NULL;
    DWORD fre_clust = 0;

    // "0:" is typical for the first FatFs volume
    FRESULT fr = f_getfree("0:", &fre_clust, &fs);
    if (fr != FR_OK || !fs) {
        s_sd_total_kb = 0;
        s_sd_free_kb  = 0;
        return;
    }

    // Number of sectors per cluster * sector size (in bytes) → KB
    uint64_t csize_sectors = (uint64_t)fs->csize;
    uint64_t sect_bytes    = 512; // FatFs default; if you configured differently, adjust.
    uint64_t cluster_kb    = (csize_sectors * sect_bytes) / 1024;

    uint64_t tot_clust = (uint64_t)(fs->n_fatent - 2);

    s_sd_total_kb = tot_clust * cluster_kb;
    s_sd_free_kb  = (uint64_t)fre_clust * cluster_kb;
}

/* Cache a readable ID from CID name (available after mount returns s_card) */
static void sd_id_update_after_mount(void)
{
    if (!s_card) { sd_id_set_unknown(); return; }

    // CID product name is typically 5 chars; not guaranteed null-terminated
    char cid_name[8] = {0};
    memcpy(cid_name, s_card->cid.name, sizeof(s_card->cid.name));
    cid_name[sizeof(s_card->cid.name)] = '\0';

    // trim trailing spaces
    for (int i = (int)strlen(cid_name) - 1; i >= 0; --i) {
        if (cid_name[i] == ' ') cid_name[i] = '\0';
        else break;
    }

    if (cid_name[0] == '\0') safe_strlcpy(s_sd_id_cached, "(no cid)", sizeof(s_sd_id_cached));
    else                     safe_strlcpy(s_sd_id_cached, cid_name, sizeof(s_sd_id_cached));
}

/* ---------------- Worker task ---------------- */
#define SD_TASK_CORE   1
#define SD_TASK_STACK  4096
#define SD_TASK_PRIO   5
#define SDQ_DEPTH      4


static QueueHandle_t s_sdq = NULL;
static TaskHandle_t  s_sd_task = NULL;

/* ---------------- Helpers ---------------- */

static void sd_helpers_init_once(void)
{
    if (!s_sd_mutex) s_sd_mutex = xSemaphoreCreateMutex();
    if (!s_sdq)      s_sdq      = xQueueCreate(SDQ_DEPTH, sizeof(sd_job_t *));
}

static void sdcard_gpio_preinit(void)
{
    gpio_reset_pin(PIN_NUM_CS);
    gpio_set_direction(PIN_NUM_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_NUM_CS, 1);
    gpio_set_pull_mode(PIN_NUM_CS, GPIO_PULLUP_ONLY);
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
    if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
}

/* ---------------- Public info ---------------- */

bool watch_sdcard_is_mounted(void)
{
    return s_mounted;
}

esp_err_t watch_sdcard_request_unmount(uint32_t timeout_ms)
{
    if (!s_sdq) return ESP_ERR_INVALID_STATE; // init not called

    sd_job_t job = {0};
    job.type = SD_JOB_UNMOUNT;
    job.timeout_ms = timeout_ms;
    job.result = ESP_FAIL;
    job.done_sem = xSemaphoreCreateBinary();
    if (!job.done_sem) return ESP_ERR_NO_MEM;

    sd_job_t *pjob = &job;

    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);

    if (xQueueSend(s_sdq, &pjob, ticks) != pdTRUE) {
        vSemaphoreDelete(job.done_sem);
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(job.done_sem, ticks ? ticks : portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(job.done_sem);
        return ESP_ERR_TIMEOUT;
    }

    vSemaphoreDelete(job.done_sem);
    // ui_show(UI_DEVICE_INFO); // refresh UI state after unmount
    return job.result;
}

const char *watch_sdcard_mount_point(void)
{
    return SDCARD_MOUNT_POINT;
}

/* ---------------- Mount/Unmount (called only from SD task) ---------------- */

esp_err_t watch_sdcard_mount(void)
{
    if (s_mounted) return ESP_OK;

    ESP_LOGI(TAG, "SD mount called on core=%d", xPortGetCoreID());

    sdcard_gpio_preinit();

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16 * 1024,
        .intr_flags = 0,
    };

    esp_err_t err = spi_bus_initialize(SDCARD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);

    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SPI3 already inited; reclaiming SPI3 bus");
        spi_bus_free(SDCARD_SPI_HOST);
        err = spi_bus_initialize(SDCARD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        sd_id_set_unmounted();
        sd_space_set_unmounted();
        return err;

    }

    s_bus_inited = true;
    s_bus_owned  = true;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SDCARD_SPI_HOST;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = SDCARD_SPI_HOST;

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(TAG, "Mounting SD card at %s ...", SDCARD_MOUNT_POINT);
    err = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_fat_sdspi_mount failed: %s", esp_err_to_name(err));

        if (s_bus_inited && s_bus_owned) spi_bus_free(SDCARD_SPI_HOST);
        s_bus_inited = false;
        s_bus_owned  = false;

        sd_id_set_unmounted();
        sd_space_set_unmounted();
        return err;

    }

    s_mounted = true;

    // cache readable SD ID for UI
    sd_id_set_unknown();
    sd_id_update_after_mount();
    sd_space_update_after_mount();

    ESP_LOGI(TAG, "SD mounted: id='%s' free=%lluKB total=%lluKB",
            s_sd_id_cached,
            (unsigned long long)s_sd_free_kb,
            (unsigned long long)s_sd_total_kb);

    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

void watch_sdcard_unmount(void)
{
    if (!s_mounted) return;

    esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;

    if (s_bus_inited) spi_bus_free(SDCARD_SPI_HOST);
    s_bus_inited = false;
    s_bus_owned  = false;

    sd_id_set_unmounted();
    sd_space_set_unmounted();

    ESP_LOGI(TAG, "SD card unmounted");
}

/* ---------------- SD worker loop ---------------- */

static void sd_task(void *arg)
{
    (void)arg;
    sd_job_t *job = NULL;

    for (;;) {
        if (xQueueReceive(s_sdq, &job, portMAX_DELAY) != pdTRUE) continue;
        if (!job) continue;

        // keep device awake while we touch SD
        screen_keep_awake_acquire();
        screen_timeout_mark_activity();

        esp_err_t e = take_lock(job->timeout_ms);
        if (e != ESP_OK) {
            job->result = e;
            screen_keep_awake_release();
            xSemaphoreGive(job->done_sem);
            continue;
        }

        if (job->type == SD_JOB_UNMOUNT) {
            // Safely unmount if mounted (serialized by mutex)
            if (s_mounted) {
                watch_sdcard_unmount();
            }
            give_lock();
            job->result = ESP_OK;

            screen_keep_awake_release();
            xSemaphoreGive(job->done_sem);
            continue;
        }

        // SD_JOB_WORK (default): mount then run work
        esp_err_t m = watch_sdcard_mount();
        if (m != ESP_OK) {
            give_lock();
            job->result = m;
            screen_keep_awake_release();
            xSemaphoreGive(job->done_sem);
            continue;
        }

        esp_err_t w = job->fn ? job->fn(job->ctx) : ESP_ERR_INVALID_ARG;

        give_lock();
        job->result = w;

        screen_keep_awake_release();
        xSemaphoreGive(job->done_sem);
    }
}


/* ---------------- Public: init + do ---------------- */

void watch_sdcard_init(void)
{
    sd_helpers_init_once();
    if (!s_sdq || !s_sd_mutex) {
        ESP_LOGE(TAG, "init failed (no mem for queue/mutex)");
        return;
    }

    if (!s_sd_task) {
        xTaskCreatePinnedToCore(sd_task, "sd_task", SD_TASK_STACK, NULL, SD_TASK_PRIO,
                                &s_sd_task, SD_TASK_CORE);
        ESP_LOGI(TAG, "SD task started on core %d", SD_TASK_CORE);
    }
}

esp_err_t watch_sdcard_do(watch_sd_work_fn_t work, void *ctx, uint32_t timeout_ms)
{
    if (!work) return ESP_ERR_INVALID_ARG;
    if (!s_sdq)  return ESP_ERR_INVALID_STATE; // init not called

    sd_job_t job = {0};
    job.type = SD_JOB_WORK; 
    job.fn = work;
    job.ctx = ctx;
    job.timeout_ms = timeout_ms;
    job.result = ESP_FAIL;
    job.done_sem = xSemaphoreCreateBinary();
    if (!job.done_sem) return ESP_ERR_NO_MEM;

    sd_job_t *pjob = &job;

    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);

    // enqueue
    if (xQueueSend(s_sdq, &pjob, ticks) != pdTRUE) {
        vSemaphoreDelete(job.done_sem);
        return ESP_ERR_TIMEOUT;
    }

    // wait completion
    if (xSemaphoreTake(job.done_sem, ticks ? ticks : portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(job.done_sem);
        return ESP_ERR_TIMEOUT;
    }

    vSemaphoreDelete(job.done_sem);
    return job.result;
}

/* ---------------- Convenience: write/read/selftest ---------------- */

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

esp_err_t watch_sdcard_write_file(const char *path, const void *data, size_t len,
                                  bool append, uint32_t timeout_ms)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    write_ctx_t ctx = { .path = path, .data = data, .len = len, .append = append };
    return watch_sdcard_do(write_work, &ctx, timeout_ms);
}

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
    if (L) {
        if (fwrite(c->line, 1, L, f) != L) { fclose(f); return ESP_FAIL; }
        if (c->line[L - 1] != '\n') {
            const char nl = '\n';
            if (fwrite(&nl, 1, 1, f) != 1) { fclose(f); return ESP_FAIL; }
        }
    } else {
        const char nl = '\n';
        if (fwrite(&nl, 1, 1, f) != 1) { fclose(f); return ESP_FAIL; }
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

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return ESP_FAIL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return ESP_FAIL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return ESP_FAIL; }

    size_t size = (size_t)sz;
    if (r->max_bytes && size > r->max_bytes) {
        fclose(f);
        ESP_LOGE(TAG, "file too big (%u > %u)", (unsigned)size, (unsigned)r->max_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = (uint8_t*)malloc(size ? size : 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }

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

esp_err_t watch_sdcard_read_file(const char *path, uint8_t **out_buf, size_t *out_len,
                                 size_t max_bytes, uint32_t timeout_ms)
{
    if (!path || !out_buf || !out_len) return ESP_ERR_INVALID_ARG;
    read_ctx_t ctx = { .path = path, .out_buf = out_buf, .out_len = out_len, .max_bytes = max_bytes };
    return watch_sdcard_do(read_work, &ctx, timeout_ms);
}

/* ---------------- Self-test ---------------- */

static esp_err_t selftest_work(void *ctx)
{
    (void)ctx;
    const char *test_path = "/sdcard/sd_test.txt";
    const char *test_data = "SDCARD_TEST_OK_123456789";

    ESP_LOGI("SD_TEST", "Writing test file...");
    FILE *f = fopen(test_path, "wb");
    if (!f) return ESP_FAIL;
    fwrite(test_data, 1, strlen(test_data), f);
    fclose(f);

    ESP_LOGI("SD_TEST", "Reading test file...");
    f = fopen(test_path, "rb");
    if (!f) return ESP_FAIL;

    char buf[64] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);

    if (n != strlen(test_data) || memcmp(buf, test_data, strlen(test_data)) != 0) {
        ESP_LOGE("SD_TEST", "Data mismatch! Read: '%s'", buf);
        return ESP_FAIL;
    }

    ESP_LOGI("SD_TEST", "Deleting test file...");
    (void)unlink(test_path);

    ESP_LOGI("SD_TEST", "SD CARD SELF-TEST OK");
    return ESP_OK;
}

esp_err_t watch_sdcard_self_test(uint32_t timeout_ms)
{
    return watch_sdcard_do(selftest_work, NULL, timeout_ms);
}
