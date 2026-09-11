#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*xiaozhi_wake_word_cb_t)(void *context);
/* recent_peak is the maximum absolute PCM value observed immediately before
 * the VAD edge.  It lets the session state machine distinguish user speech
 * from the low-level tail left by speaker playback. */
typedef void (*xiaozhi_vad_state_cb_t)(bool speech, uint32_t recent_peak, void *context);
typedef void (*xiaozhi_capture_complete_cb_t)(uint32_t rms, uint32_t peak,
                                              uint32_t frames, int64_t duration_us,
                                              void *context);
typedef void (*xiaozhi_opus_frame_cb_t)(const uint8_t *opus, size_t opus_bytes, void *context);
typedef enum {
    XIAOZHI_TTS_PLAYBACK_STARTED = 0,
    XIAOZHI_TTS_PLAYBACK_FINISHED,
    XIAOZHI_TTS_PLAYBACK_UNDERRUN,
    XIAOZHI_ERROR_TONE_PLAYBACK_FINISHED,
    XIAOZHI_LOCAL_PROMPT_PLAYBACK_FINISHED,
} xiaozhi_tts_playback_event_t;
typedef void (*xiaozhi_tts_playback_cb_t)(xiaozhi_tts_playback_event_t event,
                                          uint32_t epoch, void *context);

/* Creates the dual-core idle recognition chain. Opus work is allocated only
 * after a wake session needs uplink or cloud playback. */
esp_err_t xiaozhi_audio_pipeline_init(xiaozhi_wake_word_cb_t wake_cb,
                                      xiaozhi_vad_state_cb_t vad_cb,
                                      xiaozhi_opus_frame_cb_t opus_cb,
                                      xiaozhi_tts_playback_cb_t playback_cb,
                                      xiaozhi_capture_complete_cb_t capture_complete_cb,
                                      void *context);
/* Opens the Opus encoder before WSS/TLS and AFE allocations fragment the
 * internal heap. The encode task itself is still created on demand in PSRAM. */
esp_err_t xiaozhi_audio_pipeline_prepare_encoder(void);
void xiaozhi_audio_pipeline_deinit(void);
esp_err_t xiaozhi_audio_pipeline_set_streaming(bool enabled);
esp_err_t xiaozhi_audio_pipeline_queue_silence_opus_frames(uint32_t frame_count);
void xiaozhi_audio_pipeline_reset_wake_word(void);
/* Arms VAD for one bounded post-TTS follow-up window without requiring a
 * second wake word. The caller must restore normal wake-word mode on expiry. */
void xiaozhi_audio_pipeline_arm_followup(void);
/* Called after the follow-up state machine observes a real VAD silence edge. */
void xiaozhi_audio_pipeline_reset_after_silence(void);
/* Stop the current uplink turn and restore a clean AFE/VAD baseline while
 * leaving WakeNet armed for the next local wake. */
void xiaozhi_audio_pipeline_reset_after_transport_failure(void);
void xiaozhi_audio_pipeline_abort_playback(void);
void xiaozhi_audio_pipeline_finish_playback(void);
/* Pauses AFE/VAD detection while keeping WakeNet armed (the timer chime
 * plays through the shared playout).  Call resume_after_playback to restore. */
void xiaozhi_audio_pipeline_pause_detect(void);
esp_err_t xiaozhi_audio_pipeline_begin_tts_playback(uint32_t *epoch);
void xiaozhi_audio_pipeline_resume_after_playback(void);
esp_err_t xiaozhi_audio_pipeline_enqueue_tts_opus(const uint8_t *opus, size_t opus_bytes);
