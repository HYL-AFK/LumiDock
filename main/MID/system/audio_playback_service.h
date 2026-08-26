#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    AUDIO_PLAYOUT_SOURCE_NONE = 0,
    AUDIO_PLAYOUT_SOURCE_STARTUP,
    AUDIO_PLAYOUT_SOURCE_TTS,
} audio_playback_source_t;

typedef enum {
    AUDIO_PLAYOUT_EVENT_STARTED = 0,
    AUDIO_PLAYOUT_EVENT_FINISHED,
    AUDIO_PLAYOUT_EVENT_UNDERRUN,
} audio_playback_event_t;

typedef void (*audio_playback_event_cb_t)(audio_playback_event_t event,
                                          audio_playback_source_t source,
                                          uint32_t epoch,
                                          void *context);

esp_err_t audio_playback_service_init(void);
esp_err_t audio_playback_service_set_event_callback(audio_playback_event_cb_t callback, void *context);
esp_err_t audio_playback_service_begin(audio_playback_source_t source, uint32_t epoch,
                                       uint8_t prebuffer_frames);
esp_err_t audio_playback_service_enqueue_pcm(audio_playback_source_t source, uint32_t epoch,
                                             const uint8_t *pcm, size_t pcm_bytes);
esp_err_t audio_playback_service_finish(audio_playback_source_t source, uint32_t epoch);
void audio_playback_service_abort(audio_playback_source_t source, uint32_t epoch);
