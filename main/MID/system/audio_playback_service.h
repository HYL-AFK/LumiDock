#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    AUDIO_PLAYOUT_SOURCE_NONE = 0,
    AUDIO_PLAYOUT_SOURCE_STARTUP,
    AUDIO_PLAYOUT_SOURCE_TTS,
    AUDIO_PLAYOUT_SOURCE_RADIO,
    AUDIO_PLAYOUT_SOURCE_TIMER_CHIME,
    AUDIO_PLAYOUT_SOURCE_ERROR_TONE,
    AUDIO_PLAYOUT_SOURCE_LOCAL_PROMPT,
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
/* 当前会话还有多少帧没播出去（帧长不一，按帧数报）。
 *
 * 电台用它做自适应限速：水位见底说明拉流已经跟不上播放，这时该把限速让开去追
 * 缺口；水位够高才回到摊平节奏。只有当前会话就是这条 source/epoch 时才返回真实
 * 水位，其余情况一律报 0，所以调用方要先确认电台确实在出声（armed）再拿它下判断。 */
uint32_t audio_playback_service_pending_frames(audio_playback_source_t source, uint32_t epoch);
