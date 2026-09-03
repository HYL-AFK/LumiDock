#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ES8311 PCM format used by the XiaoZhi voice-service reference: 16 kHz,
 * mono, signed 16-bit little-endian. */
#define LUMIDOCK_AUDIO_SAMPLE_RATE_HZ 16000U
#define LUMIDOCK_AUDIO_BITS_PER_SAMPLE 16U
#define LUMIDOCK_AUDIO_CHANNELS 1U

esp_err_t i2s_audio_driver_init(void);

bool i2s_audio_driver_is_ready(void);

/* Blocking PCM APIs. They use the ES8311's simultaneously enabled RX/TX
 * channels, so independent audio tasks may record while playback is active. */
esp_err_t i2s_audio_driver_record(uint8_t *pcm, size_t pcm_bytes, size_t *bytes_read);
esp_err_t i2s_audio_driver_play(const uint8_t *pcm, size_t pcm_bytes, size_t *bytes_written);

/* Playback owns the DAC transition explicitly. PCM writes never toggle mute. */
esp_err_t i2s_audio_driver_start_output(void);
esp_err_t i2s_audio_driver_stop_output(void);

/* Keep the ES8311 DAC muted except while cloud TTS is actually playing.
 * This leaves the microphone RX path available for WakeNet. */
esp_err_t i2s_audio_driver_set_output_muted(bool muted);

/* 设置 ES8311 DAC 音量，范围 0..100，调用过程由驱动锁保护。 */
esp_err_t i2s_audio_driver_set_output_volume(float percent);
float i2s_audio_driver_get_output_volume(void);
