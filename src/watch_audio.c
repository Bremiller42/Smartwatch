#include "watch_audio.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#define TAG "AUDIO"

// Your board pins (from pincfg.h)
#define AUDIO_I2S_BCK_IO   42
#define AUDIO_I2S_LRCK_IO  2
#define AUDIO_I2S_DO_IO    41
#define AUDIO_I2S_MCK_IO   -1

typedef struct {
    uint16_t freq;
    uint16_t ms;
} beep_req_t;

static i2s_chan_handle_t s_tx = NULL;
static float s_gain = 1.0f;

static QueueHandle_t s_beep_q = NULL;
static TaskHandle_t  s_beep_task = NULL;

void watch_audio_set_gain(float gain) {
    if (gain < 0.0f) gain = 0.0f;
    s_gain = gain;
}

esp_err_t watch_audio_init(void)
{
    if (s_tx) return ESP_OK;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000), // start with 16k (very compatible)
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT,
                        I2S_SLOT_MODE_MONO        // MONO
                    ),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_I2S_BCK_IO,
            .ws   = AUDIO_I2S_LRCK_IO,
            .dout = AUDIO_I2S_DO_IO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_LOGI(TAG, "Audio I2S init at 16kHz, BCK=%d, LRCK=%d, DOUT=%d",
             AUDIO_I2S_BCK_IO, AUDIO_I2S_LRCK_IO, AUDIO_I2S_DO_IO);

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    return ESP_OK;
}


void watch_audio_deinit(void)
{
    if (!s_tx) return;
    i2s_channel_disable(s_tx);
    i2s_del_channel(s_tx);
    s_tx = NULL;
}

esp_err_t watch_audio_write_pcm16(const int16_t *pcm, int frames, int channels)
{
    if (!s_tx || !pcm || frames <= 0) return ESP_ERR_INVALID_STATE;
    if (channels != 1 && channels != 2) return ESP_ERR_INVALID_ARG;

    // Apply gain (in-place copy to a small temp chunk)
    // Keep it simple: write in chunks to avoid huge allocs.
    const int total_samples = frames * channels;
    int idx = 0;

    while (idx < total_samples) {
        int chunk = total_samples - idx;
        if (chunk > 512) chunk = 512;

        int16_t tmp[512];
        for (int i = 0; i < chunk; i++) {
            float v = (float)pcm[idx + i] * s_gain;
            if (v > 32767.f) v = 32767.f;
            if (v < -32768.f) v = -32768.f;
            tmp[i] = (int16_t)v;
        }

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_tx, tmp, chunk * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        if (err != ESP_OK) return err;

        idx += chunk;
    }

    return ESP_OK;
}

void watch_audio_beep(uint16_t freq_hz, uint16_t duration_ms)
{
    const int sample_rate = 16000;
    const int freq = freq_hz ? freq_hz : 1000;
    const int ms   = duration_ms ? duration_ms : 250;
    const int frames = (sample_rate * ms) / 1000;
    if (frames <= 0) return;

    int16_t *buf = (int16_t *)heap_caps_malloc(
        frames * sizeof(int16_t),
        MALLOC_CAP_DMA | MALLOC_CAP_8BIT
    );
    if (!buf) return;

    int n = frames;   // dynamic buffer = exact size

    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)sample_rate;
        float s = sinf(2.0f * 3.14159265f * (float)freq * t);

        // simple fade to avoid clicks (first/last 5ms)
        float env = 1.0f;
        int fade = (sample_rate * 5) / 1000;
        if (fade * 2 > n) fade = n / 2;

        if (i < fade) env = (float)i / (float)fade;
        else if (i >= (n - fade)) env = (float)(n - i - 1) / (float)fade;

        float v = s * env * 12000.0f;
        if (v > 32767.f) v = 32767.f;
        if (v < -32768.f) v = -32768.f;
        buf[i] = (int16_t)v;
    }

    ESP_LOGI(TAG, "Beep: %dHz %dms", freq, ms);

    watch_audio_write_pcm16(buf, n, 1);

    // Push a little silence so the amp/DAC doesn't hold the last sample
    int16_t zeros[256] = {0};
    for (int i = 0; i < 6; i++) {
        watch_audio_write_pcm16(zeros, 256, 1);
    }

    free(buf);
}

static void beep_task(void *arg)
{
    (void)arg;
    beep_req_t req;

    for (;;) {
        if (xQueueReceive(s_beep_q, &req, portMAX_DELAY) == pdTRUE) {
            watch_audio_beep(req.freq, req.ms);
        }
    }
}

void watch_audio_beep_async(uint16_t freq, uint16_t ms)
{
    if (!s_beep_q) return;
    beep_req_t r = { .freq = freq, .ms = ms };
    xQueueSend(s_beep_q, &r, 0); // non-blocking
}

void watch_audio_beep_async_init(void)
{
    if (s_beep_q) return;

    s_beep_q = xQueueCreate(8, sizeof(beep_req_t));
    configASSERT(s_beep_q);

    // Give this task a comfortable stack since sinf/i2s writes happen here
    xTaskCreate(beep_task, "beep_task", 4096, NULL, 5, &s_beep_task);
}