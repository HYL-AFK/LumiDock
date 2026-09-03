#include "audio_playback_service.h"

#include <stdbool.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "i2s_audio_driver.h"

#define AUDIO_PLAYOUT_PCM_INPUT_MAX_BYTES 2048U
#define AUDIO_PLAYOUT_PCM_CHUNK_BYTES 960U
#define AUDIO_PLAYOUT_TTS_PREBUFFER_FRAMES 120U
#define AUDIO_PLAYOUT_QUEUE_DEPTH 128U
#define AUDIO_PLAYOUT_STACK_BYTES 6144U
#define AUDIO_PLAYOUT_LOG_INTERVAL_US (1000000LL)

typedef struct {
    audio_playback_source_t source;
    uint32_t epoch;
    uint16_t length;
    uint8_t pcm[AUDIO_PLAYOUT_PCM_CHUNK_BYTES];
} audio_playout_frame_t;

typedef struct {
    QueueHandle_t frames;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t io_lock;
    TaskHandle_t task;
    audio_playback_event_cb_t callback;
    void *callback_context;
    audio_playback_source_t source;
    uint32_t epoch;
    uint8_t prebuffer_frames;
    bool active;
    bool started;
    bool input_closed;
    bool finish_reported;
    bool underrun_reported;
    bool started_event_reported;
    bool output_started;
} audio_playback_state_t;

static const char *TAG = "audio_playout";
static audio_playback_state_t s_playout;
static int64_t s_playout_log_us;
static uint8_t s_playout_silence[AUDIO_PLAYOUT_PCM_CHUNK_BYTES];

static void notify_event(audio_playback_event_t event, audio_playback_source_t source, uint32_t epoch)
{
    audio_playback_event_cb_t callback = NULL;
    void *context = NULL;
    xSemaphoreTake(s_playout.lock, portMAX_DELAY);
    callback = s_playout.callback;
    context = s_playout.callback_context;
    xSemaphoreGive(s_playout.lock);
    if (callback != NULL) callback(event, source, epoch, context);
}

static bool is_current(audio_playback_source_t source, uint32_t epoch)
{
    bool current = false;
    xSemaphoreTake(s_playout.lock, portMAX_DELAY);
    current = s_playout.active && s_playout.source == source && s_playout.epoch == epoch;
    xSemaphoreGive(s_playout.lock);
    return current;
}

static void discard_frames(void)
{
    audio_playout_frame_t discarded;
    while (xQueueReceive(s_playout.frames, &discarded, 0) == pdTRUE) {
    }
}

static void mute_output_serialized(void)
{
    if (s_playout.io_lock == NULL) {
        (void)i2s_audio_driver_stop_output();
        return;
    }
    xSemaphoreTake(s_playout.io_lock, portMAX_DELAY);
    (void)i2s_audio_driver_stop_output();
    xSemaphoreGive(s_playout.io_lock);
}

static void finish_current_playback(audio_playback_source_t source, uint32_t epoch)
{
    bool report = false;
    xSemaphoreTake(s_playout.lock, portMAX_DELAY);
    if (s_playout.active && s_playout.source == source && s_playout.epoch == epoch &&
        !s_playout.finish_reported) {
        s_playout.finish_reported = true;
        s_playout.active = false;
        s_playout.started = false;
        report = true;
    }
    xSemaphoreGive(s_playout.lock);
    if (!report) return;
    mute_output_serialized();
    xSemaphoreTake(s_playout.lock, portMAX_DELAY);
    s_playout.output_started = false;
    xSemaphoreGive(s_playout.lock);
    ESP_LOGI(TAG, "PCM playout drained: source=%d epoch=%u", (int)source, (unsigned)epoch);
    notify_event(AUDIO_PLAYOUT_EVENT_FINISHED, source, epoch);
}

static void audio_playback_task(void *arg)
{
    (void)arg;
    audio_playout_frame_t frame;
    for (;;) {
        audio_playback_source_t source = AUDIO_PLAYOUT_SOURCE_NONE;
        uint32_t epoch = 0U;
        uint8_t prebuffer_frames = 0U;
        bool active = false;
        bool started = false;
        bool input_closed = false;

        xSemaphoreTake(s_playout.lock, portMAX_DELAY);
        active = s_playout.active;
        started = s_playout.started;
        input_closed = s_playout.input_closed;
        source = s_playout.source;
        epoch = s_playout.epoch;
        prebuffer_frames = s_playout.prebuffer_frames;
        xSemaphoreGive(s_playout.lock);

        if (!active) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }

        const UBaseType_t queued = uxQueueMessagesWaiting(s_playout.frames);
        if (!started) {
            if (queued < prebuffer_frames && !(input_closed && queued > 0U)) {
                if (input_closed && queued == 0U) finish_current_playback(source, epoch);
                else (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
                continue;
            }
            xSemaphoreTake(s_playout.lock, portMAX_DELAY);
            if (s_playout.active && s_playout.source == source && s_playout.epoch == epoch) {
                s_playout.started = true;
            }
            xSemaphoreGive(s_playout.lock);
        }

        if (xQueueReceive(s_playout.frames, &frame, pdMS_TO_TICKS(15)) != pdTRUE) {
            xSemaphoreTake(s_playout.lock, portMAX_DELAY);
            input_closed = s_playout.input_closed;
            xSemaphoreGive(s_playout.lock);
            if (input_closed) {
                finish_current_playback(source, epoch);
                continue;
            }
            bool report_underrun = false;
            xSemaphoreTake(s_playout.lock, portMAX_DELAY);
            if (s_playout.active && s_playout.source == source && s_playout.epoch == epoch) {
                if (!s_playout.underrun_reported) {
                    s_playout.underrun_reported = true;
                    report_underrun = true;
                }
            }
            xSemaphoreGive(s_playout.lock);
            /* Keep the DAC and amplifier continuously clocked with zero PCM.
             * Toggling mute on every transient queue gap creates the audible
             * current/pop noise observed on the ES8311/NS4150B module. */
            xSemaphoreTake(s_playout.io_lock, portMAX_DELAY);
            if (i2s_audio_driver_play(s_playout_silence, sizeof(s_playout_silence), NULL) != ESP_OK) {
                ESP_LOGW(TAG, "PCM underrun silence write failed: source=%d epoch=%u",
                         (int)source, (unsigned)epoch);
            }
            xSemaphoreGive(s_playout.io_lock);
            if (report_underrun) {
                if (source == AUDIO_PLAYOUT_SOURCE_TTS) {
                    ESP_LOGW(TAG, "TTS playout underrun: source=%d epoch=%u", (int)source,
                             (unsigned)epoch);
                } else {
                    ESP_LOGW(TAG, "startup PCM playout underrun: source=%d epoch=%u",
                             (int)source, (unsigned)epoch);
                }
                notify_event(AUDIO_PLAYOUT_EVENT_UNDERRUN, source, epoch);
            }
            continue;
        }

        if (frame.source != source || frame.epoch != epoch) continue;

        bool frame_current = false;
        xSemaphoreTake(s_playout.lock, portMAX_DELAY);
        frame_current = s_playout.active && s_playout.source == source &&
                        s_playout.epoch == epoch;
        xSemaphoreGive(s_playout.lock);

        esp_err_t err = ESP_ERR_INVALID_STATE;
        int64_t write_us = 0;
        if (frame_current) {
            xSemaphoreTake(s_playout.io_lock, portMAX_DELAY);
            xSemaphoreTake(s_playout.lock, portMAX_DELAY);
            frame_current = s_playout.active && s_playout.source == source &&
                            s_playout.epoch == epoch;
            xSemaphoreGive(s_playout.lock);
            if (frame_current) {
                bool output_started = false;
                xSemaphoreTake(s_playout.lock, portMAX_DELAY);
                output_started = s_playout.output_started;
                xSemaphoreGive(s_playout.lock);
                if (!output_started) {
                    err = i2s_audio_driver_start_output();
                    if (err == ESP_OK) {
                        xSemaphoreTake(s_playout.lock, portMAX_DELAY);
                        s_playout.output_started = true;
                        xSemaphoreGive(s_playout.lock);
                    }
                } else {
                    err = ESP_OK;
                }
                const int64_t write_begin_us = esp_timer_get_time();
                if (err != ESP_FAIL) {
                    err = i2s_audio_driver_play(frame.pcm, frame.length, NULL);
                    write_us = esp_timer_get_time() - write_begin_us;
                }
            }
            xSemaphoreGive(s_playout.io_lock);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "PCM playout write failed: source=%d epoch=%u bytes=%u err=%s",
                     (int)source, (unsigned)epoch, (unsigned)frame.length, esp_err_to_name(err));
            continue;
        }

        bool report_started = false;
        xSemaphoreTake(s_playout.lock, portMAX_DELAY);
        if (s_playout.active && s_playout.source == source && s_playout.epoch == epoch &&
            !s_playout.finish_reported && s_playout.started &&
            !s_playout.started_event_reported) {
            s_playout.started_event_reported = true;
            report_started = true;
        }
        xSemaphoreGive(s_playout.lock);
        const int64_t now_us = esp_timer_get_time();
        if (report_started || now_us - s_playout_log_us >= AUDIO_PLAYOUT_LOG_INTERVAL_US) {
            ESP_LOGI(TAG, "PCM playout write: source=%d epoch=%u bytes=%u write_us=%lld queued=%u",
                     (int)source, (unsigned)epoch, (unsigned)frame.length, (long long)write_us,
                     (unsigned)uxQueueMessagesWaiting(s_playout.frames));
            s_playout_log_us = now_us;
        }
        if (report_started) {
            ESP_LOGI(TAG, "PCM playout started: source=%d epoch=%u", (int)source, (unsigned)epoch);
            notify_event(AUDIO_PLAYOUT_EVENT_STARTED, source, epoch);
        }
    }
}

esp_err_t audio_playback_service_init(void)
{
    if (s_playout.task != NULL) return ESP_OK;
    s_playout.frames = xQueueCreateWithCaps(AUDIO_PLAYOUT_QUEUE_DEPTH, sizeof(audio_playout_frame_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_playout.lock = xSemaphoreCreateMutex();
    s_playout.io_lock = xSemaphoreCreateMutex();
    if (s_playout.frames == NULL || s_playout.lock == NULL || s_playout.io_lock == NULL) {
        if (s_playout.frames != NULL) vQueueDeleteWithCaps(s_playout.frames);
        if (s_playout.lock != NULL) vSemaphoreDelete(s_playout.lock);
        if (s_playout.io_lock != NULL) vSemaphoreDelete(s_playout.io_lock);
        memset(&s_playout, 0, sizeof(s_playout));
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCoreWithCaps(audio_playback_task, "audio_playout", AUDIO_PLAYOUT_STACK_BYTES,
                                        NULL, 5, &s_playout.task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vQueueDeleteWithCaps(s_playout.frames);
        vSemaphoreDelete(s_playout.lock);
        vSemaphoreDelete(s_playout.io_lock);
        memset(&s_playout, 0, sizeof(s_playout));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "shared PCM playout ready: chunk=%u bytes threshold=%u frames (%u ms) queue=%u frames",
             (unsigned)AUDIO_PLAYOUT_PCM_CHUNK_BYTES,
             (unsigned)AUDIO_PLAYOUT_TTS_PREBUFFER_FRAMES,
             (unsigned)(AUDIO_PLAYOUT_TTS_PREBUFFER_FRAMES * 30U),
             (unsigned)AUDIO_PLAYOUT_QUEUE_DEPTH);
    return ESP_OK;
}

esp_err_t audio_playback_service_set_event_callback(audio_playback_event_cb_t callback, void *context)
{
    if (s_playout.lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_playout.lock, portMAX_DELAY);
    s_playout.callback = callback;
    s_playout.callback_context = context;
    xSemaphoreGive(s_playout.lock);
    return ESP_OK;
}

esp_err_t audio_playback_service_begin(audio_playback_source_t source, uint32_t epoch,
                                       uint8_t prebuffer_frames)
{
    if (s_playout.task == NULL || source == AUDIO_PLAYOUT_SOURCE_NONE || epoch == 0U ||
        prebuffer_frames == 0U || prebuffer_frames > AUDIO_PLAYOUT_QUEUE_DEPTH) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_playout.lock, portMAX_DELAY);
    discard_frames();
    s_playout.source = source;
    s_playout.epoch = epoch;
    s_playout.prebuffer_frames = prebuffer_frames;
    s_playout.active = true;
    s_playout.started = false;
    s_playout.input_closed = false;
    s_playout.finish_reported = false;
    s_playout.underrun_reported = false;
    s_playout.started_event_reported = false;
    s_playout.output_started = false;
    xSemaphoreGive(s_playout.lock);
    mute_output_serialized();
    xTaskNotifyGive(s_playout.task);
    return ESP_OK;
}

esp_err_t audio_playback_service_enqueue_pcm(audio_playback_source_t source, uint32_t epoch,
                                             const uint8_t *pcm, size_t pcm_bytes)
{
    if (s_playout.frames == NULL || pcm == NULL || pcm_bytes == 0U ||
        pcm_bytes > AUDIO_PLAYOUT_PCM_INPUT_MAX_BYTES || !is_current(source, epoch)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t offset = 0U;
    while (offset < pcm_bytes) {
        const size_t chunk_bytes = (pcm_bytes - offset) > AUDIO_PLAYOUT_PCM_CHUNK_BYTES
                                       ? AUDIO_PLAYOUT_PCM_CHUNK_BYTES
                                       : pcm_bytes - offset;
        audio_playout_frame_t frame = {
            .source = source,
            .epoch = epoch,
            .length = (uint16_t)chunk_bytes,
        };
        memcpy(frame.pcm, pcm + offset, chunk_bytes);
        if (xQueueSend(s_playout.frames, &frame, pdMS_TO_TICKS(250)) != pdTRUE) {
            ESP_LOGW(TAG, "PCM playout queue full: source=%d epoch=%u bytes=%u",
                     (int)source, (unsigned)epoch, (unsigned)chunk_bytes);
            return ESP_ERR_TIMEOUT;
        }
        offset += chunk_bytes;
        xTaskNotifyGive(s_playout.task);
    }
    return ESP_OK;
}

esp_err_t audio_playback_service_finish(audio_playback_source_t source, uint32_t epoch)
{
    if (!is_current(source, epoch)) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_playout.lock, portMAX_DELAY);
    s_playout.input_closed = true;
    xSemaphoreGive(s_playout.lock);
    xTaskNotifyGive(s_playout.task);
    return ESP_OK;
}

void audio_playback_service_abort(audio_playback_source_t source, uint32_t epoch)
{
    if (s_playout.lock == NULL) return;
    xSemaphoreTake(s_playout.lock, portMAX_DELAY);
    if (s_playout.active && s_playout.source == source && s_playout.epoch == epoch) {
        s_playout.active = false;
        s_playout.started = false;
        s_playout.input_closed = true;
        discard_frames();
        s_playout.output_started = false;
    }
    xSemaphoreGive(s_playout.lock);
    mute_output_serialized();
    if (s_playout.task != NULL) xTaskNotifyGive(s_playout.task);
}
