#include "radio_stream_service.h"

#include <stdbool.h>
#include <string.h>

#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio_playback_service.h"

#define RADIO_PCM_BUFFER_BYTES 4096U
#define RADIO_INPUT_BUFFER_BYTES 4096U
#define RADIO_PREBUFFER_FRAMES 12U
#define RADIO_URL "http://lhttp.qtfm.cn/live/20500038/64k.mp3"
#define RADIO_MAX_RECONNECTS 3U

static const char *TAG = "radio_stream";
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static bool s_initialized;
static bool s_play_requested;
static bool s_paused;
static bool s_active;
static uint32_t s_epoch;
static bool s_failure_pending;
static char s_failure_stage[24];
static int s_failure_code;
static uint8_t s_failure_reconnects;
static uint8_t s_reconnects;

static void record_failure(const char *stage, int code, uint8_t reconnects)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_failure_pending = true;
    strncpy(s_failure_stage, stage, sizeof(s_failure_stage) - 1U);
    s_failure_stage[sizeof(s_failure_stage) - 1U] = '\0';
    s_failure_code = code;
    s_failure_reconnects = reconnects;
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "radio failure: stage=%s code=%d reconnects=%u; returning to IDLE",
             stage, code, (unsigned)reconnects);
}

static void radio_task(void *arg)
{
    (void)arg;
    uint8_t *input = heap_caps_malloc(RADIO_INPUT_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *pcm = heap_caps_malloc(RADIO_PCM_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (input == NULL || pcm == NULL) {
        ESP_LOGE(TAG, "MP3 buffers allocation failed");
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool requested = s_play_requested;
        const bool paused = s_paused;
        uint32_t epoch = s_epoch;
        xSemaphoreGive(s_lock);
        if (!requested || paused) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }

        ESP_LOGI(TAG, "radio state=CONNECTING epoch=%u", (unsigned)epoch);
        esp_http_client_config_t cfg = {.url = RADIO_URL, .timeout_ms = 12000, .buffer_size = RADIO_INPUT_BUFFER_BYTES};
        esp_http_client_handle_t http = esp_http_client_init(&cfg);
        esp_err_t open_err = http == NULL ? ESP_ERR_NO_MEM : esp_http_client_open(http, 0);
        if (open_err != ESP_OK) {
            record_failure(http == NULL ? "http_init" : "http_open", open_err, 0);
            if (http != NULL) esp_http_client_cleanup(http);
            xSemaphoreTake(s_lock, portMAX_DELAY); s_play_requested = false; xSemaphoreGive(s_lock);
            audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_RADIO, epoch);
            continue;
        }
        (void)esp_http_client_fetch_headers(http);
        esp_audio_simple_dec_register_default();
        esp_audio_simple_dec_cfg_t dec_cfg = {.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3, .use_frame_dec = false};
        esp_audio_simple_dec_handle_t decoder = NULL;
        if (esp_audio_simple_dec_open(&dec_cfg, &decoder) != ESP_AUDIO_ERR_OK) {
            record_failure("decoder_open", ESP_FAIL, 0);
            esp_http_client_close(http); esp_http_client_cleanup(http);
            xSemaphoreTake(s_lock, portMAX_DELAY); s_play_requested = false; xSemaphoreGive(s_lock);
            audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_RADIO, epoch);
            continue;
        }
        if (audio_playback_service_begin(AUDIO_PLAYOUT_SOURCE_RADIO, epoch, RADIO_PREBUFFER_FRAMES) != ESP_OK) {
            record_failure("playback_begin", ESP_FAIL, 0);
            esp_audio_simple_dec_close(decoder); esp_http_client_close(http); esp_http_client_cleanup(http);
            xSemaphoreTake(s_lock, portMAX_DELAY); s_play_requested = false; xSemaphoreGive(s_lock);
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY); s_active = true; xSemaphoreGive(s_lock);
        size_t input_len = 0U;
        uint8_t decode_failures = 0U;
        bool retry = false;
        while (true) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            const bool keep = s_play_requested && !s_paused && s_epoch == epoch;
            xSemaphoreGive(s_lock);
            if (!keep) break;
            if (input_len == RADIO_INPUT_BUFFER_BYTES) {
                /* 解码器没有消费完整帧时保留剩余字节，避免网络读写把缓冲计数冲满。 */
                ESP_LOGW(TAG, "MP3 parser stalled with full input buffer");
                break;
            }
            int n = esp_http_client_read(http, (char *)input + input_len, RADIO_INPUT_BUFFER_BYTES - input_len);
            if (n == 0) {
                if (s_reconnects < RADIO_MAX_RECONNECTS) { ++s_reconnects; retry = true; ESP_LOGW(TAG, "radio network stall; reconnect=%u/%u", (unsigned)s_reconnects, (unsigned)RADIO_MAX_RECONNECTS); }
                else record_failure("http_read_eof", 0, s_reconnects);
                break;
            }
            if (n < 0) { record_failure("http_read", n, s_reconnects); break; }
            input_len += (size_t)n;
            esp_audio_simple_dec_raw_t raw = {.buffer = input, .len = input_len};
            esp_audio_simple_dec_out_t out = {.buffer = pcm, .len = RADIO_PCM_BUFFER_BYTES};
            const esp_audio_err_t dec_err = esp_audio_simple_dec_process(decoder, &raw, &out);
            if (dec_err == ESP_AUDIO_ERR_OK && out.decoded_size > 0U) {
                decode_failures = 0U;
                if (audio_playback_service_enqueue_pcm(AUDIO_PLAYOUT_SOURCE_RADIO, epoch, pcm, out.decoded_size) != ESP_OK) {
                    record_failure("pcm_enqueue", ESP_ERR_TIMEOUT, s_reconnects); break;
                }
            } else if (++decode_failures >= 3U) {
                record_failure("mp3_decode", dec_err, s_reconnects); break;
            }
            if (raw.consumed > 0U && raw.consumed < input_len) memmove(input, input + raw.consumed, input_len - raw.consumed);
            input_len = raw.consumed <= input_len ? input_len - raw.consumed : 0U;
        }
        audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_RADIO, epoch);
        esp_audio_simple_dec_close(decoder);
        esp_http_client_close(http); esp_http_client_cleanup(http);
        xSemaphoreTake(s_lock, portMAX_DELAY); s_active = false; xSemaphoreGive(s_lock);
        if (retry) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_play_requested && s_epoch == epoch) { ++s_epoch; if (s_epoch == 0U) s_epoch = 1U; }
            xSemaphoreGive(s_lock);
            vTaskDelay(pdMS_TO_TICKS(250));
        } else {
            xSemaphoreTake(s_lock, portMAX_DELAY); s_play_requested = false; xSemaphoreGive(s_lock);
            s_reconnects = 0U;
        }
    }
}

esp_err_t radio_stream_service_init(void)
{
    if (s_initialized) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    s_epoch = 1U;
    /* 电台持续解码不应抢占 WebSocket/TLS 所需的内部 RAM；任务栈明确放入 PSRAM。 */
    if (xTaskCreatePinnedToCoreWithCaps(radio_task, "radio_stream", 6144, NULL, 2,
                                        &s_task, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t radio_stream_service_play_default(void)
{
    if (radio_stream_service_init() != ESP_OK) return ESP_FAIL;
    xSemaphoreTake(s_lock, portMAX_DELAY); ++s_epoch; if (s_epoch == 0U) s_epoch = 1U; s_play_requested = true; s_paused = false; s_reconnects = 0U; xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    ESP_LOGI(TAG, "开始播放固定 MP3 电台");
    return ESP_OK;
}
esp_err_t radio_stream_service_pause(void) { if (!s_initialized) return ESP_ERR_INVALID_STATE; xSemaphoreTake(s_lock, portMAX_DELAY); s_paused = true; xSemaphoreGive(s_lock); return ESP_OK; }
esp_err_t radio_stream_service_resume(void) { if (!s_initialized) return ESP_ERR_INVALID_STATE; xSemaphoreTake(s_lock, portMAX_DELAY); s_paused = false; xSemaphoreGive(s_lock); xTaskNotifyGive(s_task); return ESP_OK; }
esp_err_t radio_stream_service_stop(void) { if (!s_initialized) return ESP_ERR_INVALID_STATE; xSemaphoreTake(s_lock, portMAX_DELAY); s_play_requested = false; ++s_epoch; xSemaphoreGive(s_lock); xTaskNotifyGive(s_task); return ESP_OK; }
bool radio_stream_service_is_active(void) { return s_active; }

bool radio_stream_service_consume_failure(char *stage, size_t stage_size,
                                          int *error_code, uint8_t *reconnects)
{
    if (s_lock == NULL || stage == NULL || stage_size == 0U) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool pending = s_failure_pending;
    if (pending) {
        strncpy(stage, s_failure_stage, stage_size - 1U);
        stage[stage_size - 1U] = '\0';
        if (error_code != NULL) *error_code = s_failure_code;
        if (reconnects != NULL) *reconnects = s_failure_reconnects;
        s_failure_pending = false;
    }
    xSemaphoreGive(s_lock);
    return pending;
}
