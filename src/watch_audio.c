#include "watch_audio.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"

#define TAG "AUDIO"

// Your board pins (from pincfg.h)
#define AUDIO_I2S_BCK_IO   42
#define AUDIO_I2S_LRCK_IO  2
#define AUDIO_I2S_DO_IO    41
#define AUDIO_I2S_MCK_IO   -1

// If you have an amp enable/shutdown pin, use it
#define AUDIO_AMP_EN_IO   (-1)   // set to your real pin if you have one

typedef struct {
    uint16_t freq;
    uint16_t ms;
} beep_req_t;

static i2s_chan_handle_t s_tx = NULL;
static float s_gain = 1.0f;

static QueueHandle_t s_beep_q = NULL;
static TaskHandle_t  s_beep_task = NULL;

// Gate to prevent audio activity during sleep
static volatile bool s_audio_sleeping = false;

void watch_audio_set_gain(float gain) {
    if (gain < 0.0f) gain = 0.0f;
    s_gain = gain;
}

static inline bool pin_valid_for_bitmask(int pin)
{
    return (pin >= 0) && (pin < 64);
}

static void gpio_force_low_and_optional_hold(int pin, bool hold)
{
    if (!pin_valid_for_bitmask(pin)) return;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&cfg);
    gpio_set_level(pin, 0);

    // Best-effort hold (not all pins support hold the same way on all targets)
    if (hold) gpio_hold_en((gpio_num_t)pin);
    else      gpio_hold_dis((gpio_num_t)pin);
}

static void audio_quiet_pins_hold(bool hold)
{
    // Use int so negative pins don't mess with comparisons/shifts.
    const int pins[] = {
        AUDIO_I2S_BCK_IO,
        AUDIO_I2S_LRCK_IO,
        AUDIO_I2S_DO_IO,
        AUDIO_I2S_MCK_IO,   // include if used, safe-guarded anyway
    };

    for (int i = 0; i < (int)(sizeof(pins)/sizeof(pins[0])); i++) {
        gpio_force_low_and_optional_hold(pins[i], hold);
    }
}

void watch_audio_sleep_prepare(void)
{
    s_audio_sleeping = true;

    // If I2S isn't running, just quiet pins and bail
    if (s_tx) {
        // Push some silence to ramp down
        int16_t zeros[256] = {0};
        for (int i = 0; i < 4; i++) {
            (void)watch_audio_write_pcm16(zeros, 256, 1);
        }

        // Stop I2S cleanly
        watch_audio_deinit();
    }

    // If you have amp enable pin, shut the amp down
    if (AUDIO_AMP_EN_IO >= 0) {
        gpio_force_low_and_optional_hold(AUDIO_AMP_EN_IO, true);
    }

    // Force I2S pins low and hold them through sleep (best-effort)
    audio_quiet_pins_hold(true);
}

void watch_audio_wake_restore(void)
{
    // Release holds
    audio_quiet_pins_hold(false);

    if (AUDIO_AMP_EN_IO >= 0) {
        gpio_hold_dis((gpio_num_t)AUDIO_AMP_EN_IO);
        gpio_set_level(AUDIO_AMP_EN_IO, 1); // enable amp (if active-high)
    }

    s_audio_sleeping = false;

    // Re-init I2S only when needed; don’t auto-start here
    // If you DO want it always available after wake, uncomment:
    // (void)watch_audio_init();
}

esp_err_t watch_audio_init(void)
{
    if (s_audio_sleeping) return ESP_ERR_INVALID_STATE;
    if (s_tx) return ESP_OK;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT,
                        I2S_SLOT_MODE_MONO
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
    if (s_audio_sleeping) return ESP_ERR_INVALID_STATE;
    if (!s_tx || !pcm || frames <= 0) return ESP_ERR_INVALID_STATE;
    if (channels != 1 && channels != 2) return ESP_ERR_INVALID_ARG;

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
        esp_err_t err = i2s_channel_write(
            s_tx,
            tmp,
            chunk * sizeof(int16_t),
            &bytes_written,
            portMAX_DELAY
        );
        if (err != ESP_OK) return err;

        idx += chunk;
    }

    return ESP_OK;
}

void watch_audio_beep(uint16_t freq_hz, uint16_t duration_ms)
{
    if (s_audio_sleeping) return;

    // Lazy-init I2S only if needed
    if (!s_tx) {
        if (watch_audio_init() != ESP_OK) return;
    }

    const int sample_rate = 16000;
    const int freq = freq_hz ? (int)freq_hz : 1000;
    const int ms   = duration_ms ? (int)duration_ms : 250;
    const int frames = (sample_rate * ms) / 1000;
    if (frames <= 0) return;

    // No need for DMA caps here; keep DMA RAM for actual drivers
    int16_t *buf = (int16_t *)heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_8BIT);
    if (!buf) return;

    const int n = frames;

    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)sample_rate;
        float s = sinf(2.0f * 3.14159265f * (float)freq * t);

        // Fade first/last 5ms to avoid clicks
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

    (void)watch_audio_write_pcm16(buf, n, 1);

    // Push a little silence so DAC/amp doesn't hold last sample
    int16_t zeros[256] = {0};
    for (int i = 0; i < 6; i++) {
        (void)watch_audio_write_pcm16(zeros, 256, 1);
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
    if (s_audio_sleeping) return;
    if (!s_beep_q) return;

    beep_req_t r = { .freq = freq, .ms = ms };
    (void)xQueueSend(s_beep_q, &r, 0);
}

void watch_audio_beep_async_init(void)
{
    if (s_beep_q) return;

    s_beep_q = xQueueCreate(8, sizeof(beep_req_t));
    configASSERT(s_beep_q);

    xTaskCreate(beep_task, "beep_task", 4096, NULL, 5, &s_beep_task);
}
