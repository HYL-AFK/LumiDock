#include "startup_audio.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"

#include "i2s_audio_driver.h"
#include "audio_playback_service.h"

extern const uint8_t startup_ready_pcm_start[]
    asm("_binary_startup_ready_16k_mono_s16le_pcm_start");
extern const uint8_t startup_ready_pcm_end[]
    asm("_binary_startup_ready_16k_mono_s16le_pcm_end");

static const char *TAG = "startup_audio";
static bool s_played;
static uint32_t s_startup_epoch;

/* 16 kHz x 16-bit mono x 30 ms. Smaller writes reduce scheduling jitter in
 * the shared codec path while preserving the source PCM format. */
#define STARTUP_PCM_WRITE_CHUNK_BYTES 960U

esp_err_t startup_audio_play_ready(void)
{
    if (s_played) return ESP_ERR_INVALID_STATE;
    if (!i2s_audio_driver_is_ready()) return ESP_ERR_INVALID_STATE;
    if (audio_playback_service_init() != ESP_OK) return ESP_ERR_NO_MEM;

    const size_t pcm_bytes = (size_t)(startup_ready_pcm_end - startup_ready_pcm_start);
    if (pcm_bytes == 0U || pcm_bytes > 128000U || (pcm_bytes % sizeof(int16_t)) != 0U) {
        ESP_LOGE(TAG, "embedded startup PCM invalid: bytes=%u", (unsigned)pcm_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    s_played = true;
    ++s_startup_epoch;
    if (s_startup_epoch == 0U) ++s_startup_epoch;
    // The startup asset is local, so wait for nearly two seconds of PCM before
    // opening the DAC. This prevents the boot task being delayed by Wi-Fi/TLS
    // work from starving the first few playback chunks.
    esp_err_t err = audio_playback_service_begin(AUDIO_PLAYOUT_SOURCE_STARTUP, s_startup_epoch, 64U);
    if (err != ESP_OK) return err;
    const size_t duration_ms = pcm_bytes * 1000U /
                               (LUMIDOCK_AUDIO_SAMPLE_RATE_HZ * sizeof(int16_t));
    ESP_LOGI(TAG, "startup ready PCM: %u bytes, %u ms, 16 kHz mono s16le",
             (unsigned)pcm_bytes, (unsigned)duration_ms);
    size_t offset = 0U;
    while (offset < pcm_bytes) {
        const size_t chunk_bytes = (pcm_bytes - offset) > STARTUP_PCM_WRITE_CHUNK_BYTES
                                       ? STARTUP_PCM_WRITE_CHUNK_BYTES
                                       : pcm_bytes - offset;
        err = audio_playback_service_enqueue_pcm(AUDIO_PLAYOUT_SOURCE_STARTUP, s_startup_epoch,
                                                 startup_ready_pcm_start + offset, chunk_bytes);
        if (err != ESP_OK) break;
        offset += chunk_bytes;
    }
    if (err == ESP_OK) {
        err = audio_playback_service_finish(AUDIO_PLAYOUT_SOURCE_STARTUP, s_startup_epoch);
    } else {
        audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_STARTUP, s_startup_epoch);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "startup ready PCM playback failed at %u/%u bytes: %s",
                 (unsigned)offset, (unsigned)pcm_bytes, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "startup ready PCM queued: %u chunks through shared playout",
             (unsigned)((pcm_bytes + STARTUP_PCM_WRITE_CHUNK_BYTES - 1U) /
                        STARTUP_PCM_WRITE_CHUNK_BYTES));
    return ESP_OK;
}
