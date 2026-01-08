#pragma once
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t watch_audio_init(void);
void watch_audio_deinit(void);

// Write signed 16-bit PCM to speaker.
// frames = number of samples PER CHANNEL.
esp_err_t watch_audio_write_pcm16(const int16_t *pcm, int frames, int channels);

// Simple test beep
void watch_audio_beep(uint16_t freq_hz, uint16_t duration_ms);

// Optional software gain (0.0f mute, 1.0f unity, >1 louder but can clip)
void watch_audio_set_gain(float gain);
void watch_audio_beep_async(uint16_t freq, uint16_t ms);
void watch_audio_beep_async_init(void);
void watch_audio_sleep_prepare(void);
void watch_audio_wake_restore(void);


#ifdef __cplusplus
}
#endif
