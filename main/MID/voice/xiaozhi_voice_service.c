#include "xiaozhi_voice_service.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "xiaozhi_activation_service.h"
#include "xiaozhi_audio_pipeline.h"
#include "mid_snapshot.h"
#include "voice_iot_command_service.h"

#define XIAOZHI_EVENT_QUEUE_DEPTH 12U
#define XIAOZHI_UPLINK_QUEUE_BYTES (64U * 1024U)
#define XIAOZHI_TEXT_MAX_BYTES 1024U
#define XIAOZHI_BINARY_MAX_BYTES 2048U
#define XIAOZHI_HELLO_TIMEOUT_US (8LL * 1000LL * 1000LL)
#define XIAOZHI_PREWARM_READY_TIMEOUT_US (30LL * 1000LL * 1000LL)
#define XIAOZHI_UI_ERROR_HOLD_US (2LL * 1000LL * 1000LL)
#define XIAOZHI_PREWARM_RETRY_US (5LL * 1000LL * 1000LL)
#define XIAOZHI_TRAFFIC_LOG_INTERVAL_US (1LL * 1000LL * 1000LL)
#define XIAOZHI_OPUS_FRAME_INTERVAL_US (60LL * 1000LL)
#define XIAOZHI_UPLINK_TAIL_FLUSH_FRAMES 2U
#define XIAOZHI_UPLINK_TAIL_FLUSH_TIMEOUT_US (150LL * 1000LL)
#define XIAOZHI_UPLINK_MAIN_WRITE_WAIT_TIMEOUT_US (1200LL * 1000LL)
#define XIAOZHI_WAKE_PRIMER_FRAMES 35U
#define XIAOZHI_WAKE_PRIMER_TIMEOUT_US (2600LL * 1000LL)
#define XIAOZHI_FOLLOWUP_WINDOW_US (5LL * 1000LL * 1000LL)
#define XIAOZHI_WAIT_RESPONSE_TIMEOUT_US (10LL * 1000LL * 1000LL)
#define XIAOZHI_BOOT_GREETING_TTS_TIMEOUT_US (8LL * 1000LL * 1000LL)
#define XIAOZHI_BOOT_GREETING_REPLY_WINDOW_US (180LL * 1000LL * 1000LL)
#define XIAOZHI_MAX_OPUS_FRAME_BYTES 4096U
#define XIAOZHI_BOOT_GREETING_TEXT_MAX_BYTES 384U
#define XIAOZHI_SESSION_ID_MAX_BYTES 128U
#define XIAOZHI_PIPELINE_INIT_STACK_BYTES 4096U

typedef enum {
    XIAOZHI_VOICE_WAIT_WAKE = 0,
    XIAOZHI_VOICE_WAIT_HELLO,
    XIAOZHI_VOICE_WAIT_FOLLOWUP,
    XIAOZHI_VOICE_WAIT_SPEECH,
    XIAOZHI_VOICE_LISTENING,
    XIAOZHI_VOICE_STOPPING,
    XIAOZHI_VOICE_WAIT_RESPONSE,
    XIAOZHI_VOICE_TTS_BUFFERING,
    XIAOZHI_VOICE_SPEAKING,
    XIAOZHI_VOICE_TTS_DRAINING,
    XIAOZHI_VOICE_BOOT_GREETING_WAIT_TTS,
    XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY,
} xiaozhi_voice_state_t;

typedef enum {
    VOICE_EVENT_WAKE = 0,
    VOICE_EVENT_VAD_START,
    VOICE_EVENT_VAD_END,
    VOICE_EVENT_NETWORK_LOST,
    VOICE_EVENT_WSS_CONNECTED,
    VOICE_EVENT_WSS_DISCONNECTED,
    VOICE_EVENT_WSS_TEXT,
    VOICE_EVENT_WSS_BINARY,
    VOICE_EVENT_BOOT_GREETING,
    VOICE_EVENT_TTS_PLAYBACK_STARTED,
    VOICE_EVENT_TTS_PLAYBACK_FINISHED,
    VOICE_EVENT_TTS_PLAYBACK_UNDERRUN,
    VOICE_EVENT_UPLINK_FAILED,
    VOICE_EVENT_WAKE_PRIMER_DONE,
} voice_event_type_t;

typedef struct {
    voice_event_type_t type;
    uint8_t *payload;
    size_t payload_len;
    uint32_t value;
    uint32_t detail;
} voice_event_t;

typedef struct {
    bool active;
    uint32_t connection_seq;
    uint8_t opcode;
    size_t length;
    size_t capacity;
    uint8_t *buffer;
} websocket_accumulator_t;

static const char *TAG = "xiaozhi_voice";
static esp_websocket_client_handle_t s_client;
static QueueHandle_t s_event_queue;
static RingbufHandle_t s_uplink_queue;
static SemaphoreHandle_t s_uplink_queue_lock;
static SemaphoreHandle_t s_ws_tx_lock;
static TaskHandle_t s_session_task;
static TaskHandle_t s_uplink_task;
static TaskHandle_t s_pipeline_init_task;
static TaskHandle_t s_pipeline_init_waiter;
static volatile esp_err_t s_pipeline_init_result;
/* ESP-SR maps executable model data while caches are temporarily frozen.  Keep
 * both the task stack and TCB in internal RAM, but reserve them statically so
 * the post-WSS heap fragment cannot make task creation fail. */
static StaticTask_t s_pipeline_init_tcb;
static StackType_t s_pipeline_init_stack[XIAOZHI_PIPELINE_INIT_STACK_BYTES];
static uint8_t *s_uplink_frame_buffer;
static bool s_initialized;
static bool s_pipeline_ready;
static bool s_client_started;
static bool s_hello_ready;
static bool s_vad_speech;
static bool s_wait_for_turn_silence;
static bool s_pending_wake;
static bool s_session_active;
static int64_t s_hello_deadline_us;
static xiaozhi_voice_state_t s_state;
static int64_t s_ui_idle_deadline_us;
static int64_t s_prewarm_retry_us;
static int64_t s_followup_deadline_us;
static int64_t s_response_deadline_us;
static uint32_t s_uplink_frames;
static size_t s_uplink_bytes;
static uint32_t s_uplink_drop_count;
static int64_t s_uplink_log_us;
static int64_t s_next_uplink_deadline_us;
static volatile bool s_uplink_enabled;
static volatile int64_t s_uplink_tail_deadline_us;
static volatile bool s_uplink_primer_active;
static volatile uint32_t s_uplink_primer_frames_remaining;
static volatile bool s_uplink_failed;
static volatile bool s_uplink_terminal_active;
static volatile uint32_t s_uplink_terminal_generation;
static volatile bool s_uplink_bin_in_flight;
static volatile bool s_uplink_terminal_close_pending;
static volatile bool s_uplink_terminal_listen_stop_started;
static volatile int64_t s_uplink_main_wait_us;
static const char * volatile s_uplink_tx_operation = "idle";
static uint8_t s_terminal_tail_frames[XIAOZHI_UPLINK_TAIL_FLUSH_FRAMES][XIAOZHI_MAX_OPUS_FRAME_BYTES];
static size_t s_terminal_tail_lengths[XIAOZHI_UPLINK_TAIL_FLUSH_FRAMES];
static uint8_t s_terminal_tail_count;
static uint32_t s_tts_frames;
static size_t s_tts_bytes;
static int64_t s_tts_log_us;
static int64_t s_tts_start_us;
static int64_t s_tts_last_packet_us;
static bool s_tts_first_packet_seen;
static bool s_tts_first_pcm_seen;
static int64_t s_tts_packet_log_us;
static uint32_t s_voice_event_queue_full_count;
static uint32_t s_tts_packet_drop_count;
static uint32_t s_tts_short_packet_count;
static uint32_t s_turn_id;
static uint32_t s_waiting_turn_id;
static uint32_t s_playing_turn_id;
static uint32_t s_tts_playback_epoch;
static uint32_t s_greeting_turn_id;
static uint32_t s_boot_greeting_id;
static bool s_boot_greeting_attempted;
static volatile bool s_boot_greeting_request_pending;
static bool s_playing_boot_greeting;
static bool s_playing_wake_greeting;
static bool s_listen_start_sent_for_turn;
static bool s_waiting_for_wake_primer;
static bool s_wake_primer_candidate_speech;
static int64_t s_wake_primer_deadline_us;
static bool s_boot_greeting_direct_window_active;
static int64_t s_boot_greeting_tts_deadline_us;
static int64_t s_boot_greeting_reply_deadline_us;
static bool s_discard_tts_stream;
static uint32_t s_wss_connection_seq;
static int s_last_uplink_send_result;
static bool s_last_uplink_send_connected;
static char s_session_id[XIAOZHI_SESSION_ID_MAX_BYTES];
static char s_authorization[600];
static websocket_accumulator_t s_rx;

static void notify_uplink_task(void)
{
    if (s_uplink_task != NULL) xTaskNotifyGive(s_uplink_task);
}

static void websocket_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static esp_err_t start_wss(bool foreground_session);
static void opus_frame_ready(const uint8_t *opus, size_t opus_bytes, void *context);

static void publish_ui_state(voice_ui_state_t state)
{
    const voice_session_snapshot_t snapshot = {
        .initialized = s_initialized,
        .active = s_session_active,
        .state = state,
    };
    mid_snapshot_set_voice_session(&snapshot);
}

static void schedule_ui_idle_after(int64_t hold_us)
{
    s_ui_idle_deadline_us = esp_timer_get_time() + hold_us;
}

static void free_event(voice_event_t *event)
{
    if (event->payload != NULL) heap_caps_free(event->payload);
    event->payload = NULL;
    event->payload_len = 0;
}

static bool post_event(voice_event_type_t type)
{
    if (s_event_queue == NULL) return false;
    const voice_event_t event = {.type = type, .payload = NULL, .payload_len = 0, .value = 0U};
    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        ++s_voice_event_queue_full_count;
        ESP_LOGW(TAG, "voice event queue full: %d count=%u", type,
                 (unsigned)s_voice_event_queue_full_count);
        return false;
    }
    return true;
}

static bool post_event_detail(voice_event_type_t type, uint32_t value, uint32_t detail)
{
    if (s_event_queue == NULL) return false;
    const voice_event_t event = {.type = type, .payload = NULL, .payload_len = 0, .value = value,
                                 .detail = detail};
    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        ++s_voice_event_queue_full_count;
        ESP_LOGW(TAG, "voice event queue full: %d value=%u detail=%u count=%u", type,
                 (unsigned)value, (unsigned)detail, (unsigned)s_voice_event_queue_full_count);
        return false;
    }
    return true;
}

static bool post_event_value(voice_event_type_t type, uint32_t value)
{
    return post_event_detail(type, value, 0U);
}

static bool post_wss_disconnect_event(uint32_t connection_seq, int32_t event_id)
{
    return post_event_detail(VOICE_EVENT_WSS_DISCONNECTED, connection_seq, (uint32_t)event_id);
}

static const char *wss_event_name(int32_t event_id)
{
    if (event_id == WEBSOCKET_EVENT_ERROR) return "ERROR";
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED) return "DISCONNECTED";
    if (event_id == WEBSOCKET_EVENT_CLOSED) return "CLOSED";
    return "OTHER";
}

static void post_owned_event_value(voice_event_type_t type, uint8_t *payload, size_t payload_len,
                                   uint32_t value)
{
    if (payload == NULL || payload_len == 0U || s_event_queue == NULL) {
        heap_caps_free(payload);
        return;
    }
    const voice_event_t event = {.type = type, .payload = payload, .payload_len = payload_len, .value = value};
    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        ++s_voice_event_queue_full_count;
        ESP_LOGW(TAG, "voice event queue full: %d count=%u", type,
                 (unsigned)s_voice_event_queue_full_count);
        heap_caps_free(payload);
    }
}

static void post_playback_event(voice_event_type_t type, uint32_t epoch)
{
    if (s_event_queue == NULL) return;
    const voice_event_t event = {.type = type, .payload = NULL, .payload_len = 0, .value = epoch};
    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        ++s_voice_event_queue_full_count;
        ESP_LOGW(TAG, "voice event queue full: playback event=%d count=%u", type,
                 (unsigned)s_voice_event_queue_full_count);
    }
}

static void rx_reset(void)
{
    heap_caps_free(s_rx.buffer);
    memset(&s_rx, 0, sizeof(s_rx));
}

static void cleanup_init_failure(void)
{
    s_uplink_enabled = false;
    s_uplink_terminal_active = true;
    s_uplink_terminal_close_pending = false;
    s_uplink_terminal_listen_stop_started = false;
    notify_uplink_task();
    if (s_uplink_task != NULL) vTaskDeleteWithCaps(s_uplink_task);
    s_uplink_task = NULL;
    if (s_pipeline_init_task != NULL) vTaskDelete(s_pipeline_init_task);
    s_pipeline_init_task = NULL;
    s_pipeline_init_waiter = NULL;
    heap_caps_free(s_uplink_frame_buffer);
    s_uplink_frame_buffer = NULL;
    xiaozhi_audio_pipeline_deinit();
    if (s_client != NULL) {
        if (s_client_started) (void)esp_websocket_client_stop(s_client);
        (void)esp_websocket_client_destroy(s_client);
        s_client = NULL;
        s_client_started = false;
    }
    if (s_session_task != NULL) vTaskDeleteWithCaps(s_session_task);
    if (s_event_queue != NULL) vQueueDelete(s_event_queue);
    if (s_uplink_queue != NULL) vRingbufferDeleteWithCaps(s_uplink_queue);
    if (s_uplink_queue_lock != NULL) vSemaphoreDelete(s_uplink_queue_lock);
    if (s_ws_tx_lock != NULL) vSemaphoreDelete(s_ws_tx_lock);
    rx_reset();
    s_event_queue = NULL;
    s_uplink_queue = NULL;
    s_uplink_queue_lock = NULL;
    s_ws_tx_lock = NULL;
    s_session_task = NULL;
    s_pipeline_ready = false;
    s_initialized = false;
}

static void receive_websocket_payload(const esp_websocket_event_data_t *event, uint32_t connection_seq)
{
    if (event == NULL || event->data_ptr == NULL || event->data_len <= 0) return;
    if (connection_seq != s_wss_connection_seq) {
        ESP_LOGI(TAG, "stale WSS data ignored: event_seq=%u current_seq=%u",
                 (unsigned)connection_seq, (unsigned)s_wss_connection_seq);
        return;
    }
    const bool continuation = event->op_code == WS_TRANSPORT_OPCODES_CONT;
    if (!s_rx.active) {
        if (continuation) {
            ESP_LOGW(TAG, "orphan WSS continuation ignored");
            return;
        }
        const size_t capacity = event->op_code == WS_TRANSPORT_OPCODES_TEXT ?
                                    XIAOZHI_TEXT_MAX_BYTES : XIAOZHI_BINARY_MAX_BYTES;
        if (event->payload_len > (int)capacity || event->data_len > (int)capacity) {
            ESP_LOGW(TAG, "WSS payload exceeds cap: %d", event->payload_len);
            return;
        }
        s_rx.buffer = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_rx.buffer == NULL) {
            ESP_LOGW(TAG, "no PSRAM for WSS payload");
            return;
        }
        s_rx.active = true;
        s_rx.connection_seq = connection_seq;
        s_rx.opcode = event->op_code;
        s_rx.capacity = capacity;
    } else if (s_rx.connection_seq != connection_seq || (!continuation && event->payload_offset == 0)) {
        rx_reset();
        receive_websocket_payload(event, connection_seq);
        return;
    }

    if ((size_t)event->data_len > s_rx.capacity - s_rx.length) {
        ESP_LOGW(TAG, "WSS reassembly overflow");
        rx_reset();
        return;
    }
    memcpy(s_rx.buffer + s_rx.length, event->data_ptr, (size_t)event->data_len);
    s_rx.length += (size_t)event->data_len;

    const bool end_of_payload = event->payload_len == 0 ||
                                ((size_t)event->payload_offset + (size_t)event->data_len >=
                                 (size_t)event->payload_len);
    if (!event->fin || !end_of_payload) return;

    const voice_event_type_t type = s_rx.opcode == WS_TRANSPORT_OPCODES_TEXT ?
                                        VOICE_EVENT_WSS_TEXT : VOICE_EVENT_WSS_BINARY;
    uint8_t *payload = s_rx.buffer;
    const size_t payload_len = s_rx.length;
    memset(&s_rx, 0, sizeof(s_rx));
    post_owned_event_value(type, payload, payload_len, connection_seq);
}

static uint32_t discard_uplink_queue(void)
{
    uint32_t discarded = 0U;
    if (s_uplink_queue == NULL) return discarded;
    if (s_uplink_queue_lock != NULL) xSemaphoreTake(s_uplink_queue_lock, portMAX_DELAY);
    for (;;) {
        size_t item_size = 0U;
        void *item = xRingbufferReceive(s_uplink_queue, &item_size, 0);
        if (item == NULL) break;
        vRingbufferReturnItem(s_uplink_queue, item);
        ++discarded;
    }
    if (s_uplink_queue_lock != NULL) xSemaphoreGive(s_uplink_queue_lock);
    return discarded;
}

static void reset_terminal_uplink_for_turn(void)
{
    if (s_uplink_queue_lock != NULL) xSemaphoreTake(s_uplink_queue_lock, portMAX_DELAY);
    s_uplink_terminal_active = false;
    s_uplink_terminal_close_pending = false;
    s_uplink_terminal_listen_stop_started = false;
    s_terminal_tail_count = 0U;
    if (s_uplink_queue_lock != NULL) xSemaphoreGive(s_uplink_queue_lock);
}

static void begin_terminal_uplink_transition(void)
{
    if (s_uplink_queue_lock != NULL) xSemaphoreTake(s_uplink_queue_lock, portMAX_DELAY);
    s_uplink_terminal_active = true;
    s_uplink_terminal_close_pending = false;
    s_uplink_terminal_listen_stop_started = false;
    ++s_uplink_terminal_generation;
    if (s_uplink_terminal_generation == 0U) ++s_uplink_terminal_generation;
    s_uplink_enabled = false;
    if (s_uplink_queue_lock != NULL) xSemaphoreGive(s_uplink_queue_lock);
    ESP_LOGI(TAG, "terminal uplink begin: turn=%u terminal_gen=%u",
             (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation);
}

static bool begin_main_uplink_send(void)
{
    bool admitted = false;
    if (s_uplink_queue_lock != NULL) xSemaphoreTake(s_uplink_queue_lock, portMAX_DELAY);
    if (!s_uplink_terminal_active && (s_uplink_enabled || s_uplink_primer_active)) {
        s_uplink_bin_in_flight = true;
        admitted = true;
    }
    if (s_uplink_queue_lock != NULL) xSemaphoreGive(s_uplink_queue_lock);
    return admitted;
}

static bool finish_main_uplink_send(void)
{
    if (s_uplink_queue_lock != NULL) xSemaphoreTake(s_uplink_queue_lock, portMAX_DELAY);
    s_uplink_bin_in_flight = false;
    const bool terminal_deadline = s_uplink_terminal_close_pending;
    s_uplink_terminal_close_pending = false;
    if (s_uplink_queue_lock != NULL) xSemaphoreGive(s_uplink_queue_lock);
    return terminal_deadline;
}

static bool wait_for_main_uplink_write(void)
{
    const int64_t wait_start_us = esp_timer_get_time();
    bool tail_deadline_logged = false;
    for (;;) {
        if (s_uplink_queue_lock != NULL) xSemaphoreTake(s_uplink_queue_lock, portMAX_DELAY);
        const bool in_flight = s_uplink_bin_in_flight;
        const int64_t now_us = esp_timer_get_time();
        if (in_flight && !tail_deadline_logged && now_us >= s_uplink_tail_deadline_us) {
            s_uplink_terminal_close_pending = true;
            tail_deadline_logged = true;
            ESP_LOGW(TAG, "terminal tail cutoff passed while bin-main in flight: turn=%u terminal_gen=%u wait_us=%lld in_flight=1 send_result=%d connected=%d",
                     (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation,
                     (long long)(now_us - wait_start_us), s_last_uplink_send_result,
                     s_last_uplink_send_connected ? 1 : 0);
        }
        if (s_uplink_queue_lock != NULL) xSemaphoreGive(s_uplink_queue_lock);
        if (!in_flight) {
            s_uplink_main_wait_us = now_us - wait_start_us;
            ESP_LOGI(TAG, "in-flight bin-main completed: turn=%u terminal_gen=%u wait_us=%lld in_flight=0 send_result=%d connected=%d after_tail_cutoff=%d",
                     (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation,
                     (long long)(now_us - wait_start_us), s_last_uplink_send_result,
                     s_last_uplink_send_connected ? 1 : 0, tail_deadline_logged ? 1 : 0);
            return true;
        }
        if (now_us - wait_start_us >= XIAOZHI_UPLINK_MAIN_WRITE_WAIT_TIMEOUT_US) {
            s_uplink_main_wait_us = now_us - wait_start_us;
            ESP_LOGE(TAG, "terminal main write wait timed out: turn=%u terminal_gen=%u in_flight=%d wait_us=%lld send_result=%d connected=%d",
                     (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation,
                     in_flight ? 1 : 0, (long long)(now_us - wait_start_us),
                     s_last_uplink_send_result, s_last_uplink_send_connected ? 1 : 0);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void log_uplink_queue_depth(const char *phase)
{
    if (s_uplink_queue == NULL) return;
    size_t items_waiting = 0U;
    size_t free_size = 0U;
    if (s_uplink_queue_lock != NULL) xSemaphoreTake(s_uplink_queue_lock, portMAX_DELAY);
    vRingbufferGetInfo(s_uplink_queue, NULL, NULL, NULL, NULL, &items_waiting);
    free_size = xRingbufferGetCurFreeSize(s_uplink_queue);
    if (s_uplink_queue_lock != NULL) xSemaphoreGive(s_uplink_queue_lock);
    ESP_LOGI(TAG, "Opus uplink queue depth: phase=%s items=%u free_bytes=%u terminal_gen=%u",
             phase, (unsigned)items_waiting, (unsigned)free_size,
             (unsigned)s_uplink_terminal_generation);
}

static bool capture_terminal_uplink_tails(void)
{
    uint32_t discarded = 0U;
    s_terminal_tail_count = 0U;
    if (s_uplink_queue == NULL) return false;
    log_uplink_queue_depth("terminal-capture-before");
    if (s_uplink_queue_lock != NULL) xSemaphoreTake(s_uplink_queue_lock, portMAX_DELAY);
    for (;;) {
        size_t item_size = 0U;
        uint8_t *item = xRingbufferReceive(s_uplink_queue, &item_size, 0);
        if (item == NULL) break;
        if (item_size == 0U || item_size > XIAOZHI_MAX_OPUS_FRAME_BYTES) {
            ++discarded;
        } else if (s_terminal_tail_count < XIAOZHI_UPLINK_TAIL_FLUSH_FRAMES) {
            const uint8_t tail_index = s_terminal_tail_count++;
            memcpy(s_terminal_tail_frames[tail_index], item, item_size);
            s_terminal_tail_lengths[tail_index] = item_size;
        } else {
            memcpy(s_terminal_tail_frames[0], s_terminal_tail_frames[1],
                   s_terminal_tail_lengths[1]);
            s_terminal_tail_lengths[0] = s_terminal_tail_lengths[1];
            memcpy(s_terminal_tail_frames[1], item, item_size);
            s_terminal_tail_lengths[1] = item_size;
            ++discarded;
        }
        vRingbufferReturnItem(s_uplink_queue, item);
    }
    if (s_uplink_queue_lock != NULL) xSemaphoreGive(s_uplink_queue_lock);
    ESP_LOGI(TAG, "terminal uplink captured: turn=%u terminal_gen=%u dropped_generic=%u retained_tails=%u",
             (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation,
             (unsigned)discarded, (unsigned)s_terminal_tail_count);
    log_uplink_queue_depth("terminal-capture-after");
    return true;
}

static void reset_uplink_pacing(void)
{
    s_next_uplink_deadline_us = 0;
}

static void advance_uplink_pacing(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (s_next_uplink_deadline_us == 0) {
        s_next_uplink_deadline_us = now_us + XIAOZHI_OPUS_FRAME_INTERVAL_US;
        return;
    }
    s_next_uplink_deadline_us += XIAOZHI_OPUS_FRAME_INTERVAL_US;
    if (s_next_uplink_deadline_us <= now_us) {
        s_next_uplink_deadline_us = now_us + XIAOZHI_OPUS_FRAME_INTERVAL_US;
    }
}

static bool send_ws_text_raw(const char *text)
{
    if (text == NULL || s_ws_tx_lock == NULL) return false;
    xSemaphoreTake(s_ws_tx_lock, portMAX_DELAY);
    const bool connected = s_client != NULL && esp_websocket_client_is_connected(s_client);
    const int result = connected
                           ? esp_websocket_client_send_text(s_client, text, strlen(text), pdMS_TO_TICKS(500))
                           : 0;
    ESP_LOGI(TAG, "WSS text send: result=%d session=%s turn=%u payload=%s", result,
             s_session_id[0] != '\0' ? s_session_id : "-", (unsigned)s_turn_id,
             text != NULL ? text : "-");
    xSemaphoreGive(s_ws_tx_lock);
    if (result <= 0) {
        ESP_LOGW(TAG, "cloud text send result=%d", result);
        return false;
    }
    return true;
}

static bool send_text(const char *text)
{
    if (!s_hello_ready) return false;
    return send_ws_text_raw(text);
}

static void log_uplink_traffic(size_t bytes)
{
    ++s_uplink_frames;
    s_uplink_bytes += bytes;
    const int64_t now_us = esp_timer_get_time();
    if (now_us - s_uplink_log_us < XIAOZHI_TRAFFIC_LOG_INTERVAL_US) return;
    ESP_LOGI(TAG, "voice uplink: frames=%u bytes=%u", (unsigned)s_uplink_frames,
             (unsigned)s_uplink_bytes);
    s_uplink_frames = 0;
    s_uplink_bytes = 0U;
    s_uplink_log_us = now_us;
}

static void log_tts_traffic(size_t bytes)
{
    ++s_tts_frames;
    s_tts_bytes += bytes;
    const int64_t now_us = esp_timer_get_time();
    if (now_us - s_tts_log_us < XIAOZHI_TRAFFIC_LOG_INTERVAL_US) return;
    ESP_LOGI(TAG, "cloud TTS: frames=%u bytes=%u", (unsigned)s_tts_frames,
             (unsigned)s_tts_bytes);
    s_tts_frames = 0;
    s_tts_bytes = 0U;
    s_tts_log_us = now_us;
}

static void close_channel(bool refresh_credentials)
{
    const bool had_active_session = s_session_active;
    s_uplink_enabled = false;
    s_uplink_terminal_active = true;
    s_uplink_terminal_close_pending = false;
    s_uplink_terminal_listen_stop_started = false;
    s_terminal_tail_count = 0U;
    s_uplink_primer_active = false;
    s_uplink_primer_frames_remaining = 0U;
    s_uplink_failed = false;
    notify_uplink_task();
    (void)discard_uplink_queue();
    xiaozhi_audio_pipeline_reset_wake_word();
    xiaozhi_audio_pipeline_reset_after_silence();
    xiaozhi_audio_pipeline_abort_playback();
    s_hello_ready = false;
    s_pending_wake = false;
    s_wait_for_turn_silence = false;
    s_hello_deadline_us = 0;
    s_followup_deadline_us = 0;
    s_response_deadline_us = 0;
    s_waiting_turn_id = 0U;
    s_playing_turn_id = 0U;
    s_tts_playback_epoch = 0U;
    s_greeting_turn_id = 0U;
    s_boot_greeting_id = 0U;
    s_boot_greeting_request_pending = false;
    s_playing_boot_greeting = false;
    s_playing_wake_greeting = false;
    s_listen_start_sent_for_turn = false;
    s_waiting_for_wake_primer = false;
    s_wake_primer_candidate_speech = false;
    s_wake_primer_deadline_us = 0;
    s_boot_greeting_direct_window_active = false;
    s_boot_greeting_tts_deadline_us = 0;
    s_boot_greeting_reply_deadline_us = 0;
    s_discard_tts_stream = false;
    s_session_id[0] = '\0';
    s_tts_start_us = 0;
    s_tts_last_packet_us = 0;
    s_tts_first_packet_seen = false;
    reset_uplink_pacing();
    s_state = XIAOZHI_VOICE_WAIT_WAKE;
    s_session_active = false;
    if (had_active_session) {
        publish_ui_state(VOICE_UI_ERROR);
        schedule_ui_idle_after(XIAOZHI_UI_ERROR_HOLD_US);
    } else {
        publish_ui_state(VOICE_UI_IDLE);
    }
    if (s_client != NULL) {
        if (s_ws_tx_lock != NULL) xSemaphoreTake(s_ws_tx_lock, portMAX_DELAY);
        if (s_client_started) (void)esp_websocket_client_stop(s_client);
        (void)esp_websocket_client_destroy(s_client);
        s_client = NULL;
        s_client_started = false;
        if (s_ws_tx_lock != NULL) xSemaphoreGive(s_ws_tx_lock);
    }
    if (refresh_credentials) xiaozhi_activation_service_refresh_credentials();
    if (s_initialized && xiaozhi_activation_service_is_activated()) {
        s_prewarm_retry_us = esp_timer_get_time() + XIAOZHI_PREWARM_RETRY_US;
        ESP_LOGI(TAG, "WSS idle prewarm scheduled: delay_ms=%u", (unsigned)(XIAOZHI_PREWARM_RETRY_US / 1000LL));
    }
}

static bool send_uplink_packet(const uint8_t *opus, size_t opus_len, const char *operation)
{
    if (s_ws_tx_lock == NULL) return false;
    s_uplink_tx_operation = operation;
    xSemaphoreTake(s_ws_tx_lock, portMAX_DELAY);
    s_last_uplink_send_connected = s_client != NULL && esp_websocket_client_is_connected(s_client);
    s_last_uplink_send_result = s_last_uplink_send_connected
                                    ? esp_websocket_client_send_bin(
                                          s_client, (const char *)opus, (int)opus_len,
                                          pdMS_TO_TICKS(1000))
                                    : 0;
    xSemaphoreGive(s_ws_tx_lock);
    ESP_LOGI(TAG, "Opus uplink op=%s result=%d connected=%d state=%d turn=%u terminal_gen=%u bytes=%u timeout_ms=1000",
             operation,
             s_last_uplink_send_result, s_last_uplink_send_connected ? 1 : 0,
             s_state, (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation,
             (unsigned)opus_len);
    s_uplink_tx_operation = "idle";
    if (s_last_uplink_send_result > 0) return true;
    ESP_LOGE(TAG, "voice turn transport failure: op=%s turn=%u frames=%u connected=%d result=%d",
             operation,
             (unsigned)s_turn_id, (unsigned)s_uplink_frames,
             s_last_uplink_send_connected ? 1 : 0, s_last_uplink_send_result);
    return false;
}

static bool send_listen_start(void)
{
    if (s_session_id[0] == '\0') {
        ESP_LOGW(TAG, "listen/start not sent: current WSS session_id is missing");
        return false;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL || cJSON_AddStringToObject(root, "type", "listen") == NULL ||
        cJSON_AddStringToObject(root, "state", "start") == NULL ||
        cJSON_AddStringToObject(root, "mode", "manual") == NULL ||
        cJSON_AddStringToObject(root, "session_id", s_session_id) == NULL) {
        cJSON_Delete(root);
        return false;
    }
    char *request = cJSON_PrintUnformatted(root);
    const bool sent = request != NULL && send_text(request);
    ESP_LOGI(TAG, "listen/start sent: mode=manual session_id=%s turn=%u ok=%d",
             s_session_id, (unsigned)s_turn_id, sent ? 1 : 0);
    cJSON_free(request);
    cJSON_Delete(root);
    return sent;
}

static bool send_listen_stop(void)
{
    if (s_session_id[0] == '\0') {
        ESP_LOGW(TAG, "listen/stop not sent: current WSS session_id is missing");
        return false;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL || cJSON_AddStringToObject(root, "type", "listen") == NULL ||
        cJSON_AddStringToObject(root, "state", "stop") == NULL ||
        cJSON_AddStringToObject(root, "session_id", s_session_id) == NULL) {
        cJSON_Delete(root);
        return false;
    }
    char *request = cJSON_PrintUnformatted(root);
    const bool sent = request != NULL && send_text(request);
    ESP_LOGI(TAG, "listen/stop sent: session_id=%s turn=%u ok=%d",
             s_session_id, (unsigned)s_turn_id, sent ? 1 : 0);
    cJSON_free(request);
    cJSON_Delete(root);
    return sent;
}

static bool start_wake_server_ignore_primer(void)
{
    if (!s_listen_start_sent_for_turn) {
        ++s_turn_id;
        if (s_turn_id == 0U) ++s_turn_id;
        s_waiting_turn_id = 0U;
        if (!send_listen_start()) return false;
        s_listen_start_sent_for_turn = true;
        reset_uplink_pacing();
        (void)discard_uplink_queue();
        s_uplink_failed = false;
        reset_terminal_uplink_for_turn();
    }
    s_uplink_enabled = false;
    s_uplink_primer_active = true;
    s_uplink_primer_frames_remaining = XIAOZHI_WAKE_PRIMER_FRAMES;
    s_waiting_for_wake_primer = true;
    s_wake_primer_candidate_speech = false;
    s_wake_primer_deadline_us = esp_timer_get_time() + XIAOZHI_WAKE_PRIMER_TIMEOUT_US;
    if (xiaozhi_audio_pipeline_queue_silence_opus_frames(XIAOZHI_WAKE_PRIMER_FRAMES) != ESP_OK) {
        ESP_LOGW(TAG, "wake server ignore primer could not be queued");
        return false;
    }
    ESP_LOGI(TAG, "wake server ignore primer started: frames=%u duration_ms=%u turn=%u",
             (unsigned)XIAOZHI_WAKE_PRIMER_FRAMES,
             (unsigned)(XIAOZHI_WAKE_PRIMER_FRAMES * 60U), (unsigned)s_turn_id);
    notify_uplink_task();
    return true;
}

static void start_listen(bool enable_capture)
{
    if (s_state != XIAOZHI_VOICE_WAIT_SPEECH) return;
    if (!s_listen_start_sent_for_turn) {
        ++s_turn_id;
        if (s_turn_id == 0U) ++s_turn_id;
        s_waiting_turn_id = 0U;
        s_playing_turn_id = 0U;
        if (!send_listen_start()) {
            ESP_LOGW(TAG, "listen/start send failed; ending current turn; wake word required");
            xiaozhi_audio_pipeline_reset_after_transport_failure();
            close_channel(false);
            return;
        }
        s_listen_start_sent_for_turn = true;
        reset_uplink_pacing();
        (void)discard_uplink_queue();
        s_uplink_failed = false;
        reset_terminal_uplink_for_turn();
    }
    if (!enable_capture) {
        if (!start_wake_server_ignore_primer()) {
            close_channel(false);
        }
        return;
    }
    s_uplink_primer_active = false;
    s_uplink_primer_frames_remaining = 0U;
    s_uplink_enabled = true;
    notify_uplink_task();
    if (xiaozhi_audio_pipeline_set_streaming(true) != ESP_OK) {
        ESP_LOGW(TAG, "unable to start Opus uplink; closing this voice session");
        close_channel(false);
        return;
    }
    s_state = XIAOZHI_VOICE_LISTENING;
    ESP_LOGI(TAG, "voice VAD: speech started; Opus uplink enabled turn=%u",
             (unsigned)s_turn_id);
    publish_ui_state(VOICE_UI_LISTENING);
}

static void finish_boot_greeting_failure(const char *reason)
{
    xiaozhi_audio_pipeline_abort_playback();
    xiaozhi_audio_pipeline_reset_wake_word();
    s_boot_greeting_id = 0U;
    s_playing_boot_greeting = false;
    s_boot_greeting_direct_window_active = false;
    s_boot_greeting_tts_deadline_us = 0;
    s_boot_greeting_reply_deadline_us = 0;
    s_response_deadline_us = 0;
    s_wait_for_turn_silence = false;
    s_waiting_turn_id = 0U;
    s_playing_turn_id = 0U;
    s_tts_playback_epoch = 0U;
    s_discard_tts_stream = false;
    s_state = XIAOZHI_VOICE_WAIT_WAKE;
    s_session_active = false;
    ESP_LOGW(TAG, "boot greeting failed: %s; ordinary wake word remains available", reason);
    publish_ui_state(VOICE_UI_ERROR);
    schedule_ui_idle_after(XIAOZHI_UI_ERROR_HOLD_US);
}

static const char *boot_greeting_salutation(int hour)
{
    if (hour < 11) return "早上好";
    if (hour < 14) return "中午好";
    if (hour < 18) return "下午好";
    return "晚上好";
}

static bool build_boot_greeting_text(char *greeting, size_t greeting_size)
{
    lumidock_snapshot_t snapshot = {0};
    mid_snapshot_get(&snapshot);

    if (!snapshot.time_weather.valid || snapshot.time_weather.weather_text[0] == '\0') {
        const int written = snprintf(greeting, greeting_size,
                                     "[device_call]你好呀，很高兴见到你，今天有什么想和我聊的吗？");
        return written >= 0 && (size_t)written < greeting_size;
    }

    const struct tm *local_time = &snapshot.time_weather.local_time;
    const int temperature_c = snapshot.time_weather.temperature_c >= 0.0f ?
                                  (int)(snapshot.time_weather.temperature_c + 0.5f) :
                                  (int)(snapshot.time_weather.temperature_c - 0.5f);
    const uint32_t template_index = (uint32_t)snapshot.time_weather.local_time.tm_yday % 3U;
    const char *weather = snapshot.time_weather.weather_text;
    int written;
    if (template_index == 0U) {
        written = snprintf(greeting, greeting_size,
                           "[device_call]%s，现在是%d点%02d分，今天%s，气温%d度。今天有什么安排吗？",
                           boot_greeting_salutation(local_time->tm_hour), local_time->tm_hour,
                           local_time->tm_min, weather, temperature_c);
    } else if (template_index == 1U) {
        written = snprintf(greeting, greeting_size,
                           "[device_call]今天是星期%d，当前%d点%02d分，天气%s，温度%d度。出门记得留意天气哦。",
                           local_time->tm_wday, local_time->tm_hour, local_time->tm_min,
                           weather, temperature_c);
    } else {
        written = snprintf(greeting, greeting_size,
                           "[device_call]你好呀，现在%d点%02d分，今天%s，%d度。想让我帮你查点什么吗？",
                           local_time->tm_hour, local_time->tm_min, weather, temperature_c);
    }
    return written >= 0 && (size_t)written < greeting_size;
}

static bool send_boot_greeting_detect(void)
{
    char greeting[XIAOZHI_BOOT_GREETING_TEXT_MAX_BYTES] = {0};
    if (!build_boot_greeting_text(greeting, sizeof(greeting))) {
        ESP_LOGW(TAG, "boot greeting text generation failed");
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL || cJSON_AddStringToObject(root, "type", "listen") == NULL ||
        cJSON_AddStringToObject(root, "state", "detect") == NULL ||
        cJSON_AddStringToObject(root, "text", greeting) == NULL) {
        cJSON_Delete(root);
        return false;
    }
    char *request = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (request == NULL) return false;

    ESP_LOGI(TAG, "boot greeting text prepared: %s", greeting);
    const bool sent = send_text(request);
    cJSON_free(request);
    return sent;
}

static void begin_boot_greeting(void)
{
    s_boot_greeting_request_pending = false;
    if (s_boot_greeting_attempted) return;
    s_boot_greeting_attempted = true;
    if (!s_hello_ready || s_client == NULL || !esp_websocket_client_is_connected(s_client)) {
        finish_boot_greeting_failure("WSS not ready");
        return;
    }
    if (!send_boot_greeting_detect()) {
        finish_boot_greeting_failure("listen/detect send failed");
        return;
    }
    ++s_turn_id;
    if (s_turn_id == 0U) ++s_turn_id;
    s_boot_greeting_id = s_turn_id;
    s_waiting_turn_id = 0U;
    s_playing_turn_id = 0U;
    s_session_active = true;
    s_state = XIAOZHI_VOICE_BOOT_GREETING_WAIT_TTS;
    s_boot_greeting_tts_deadline_us = esp_timer_get_time() + XIAOZHI_BOOT_GREETING_TTS_TIMEOUT_US;
    s_boot_greeting_reply_deadline_us = 0;
    ESP_LOGI(TAG, "boot greeting detect sent: id=%u; waiting for one cloud TTS", (unsigned)s_boot_greeting_id);
    publish_ui_state(VOICE_UI_CONNECTING);
}

static bool send_terminal_uplink_sequence(void)
{
    const uint32_t terminal_generation = s_uplink_terminal_generation;
    for (uint8_t index = 0U; index < s_terminal_tail_count; ++index) {
        if (esp_timer_get_time() >= s_uplink_tail_deadline_us) {
            ESP_LOGI(TAG, "terminal uplink deadline reached: turn=%u terminal_gen=%u sent_tails=%u retained_tails=%u",
                     (unsigned)s_turn_id, (unsigned)terminal_generation, (unsigned)index,
                     (unsigned)s_terminal_tail_count);
            break;
        }
        if (!send_uplink_packet(s_terminal_tail_frames[index], s_terminal_tail_lengths[index], "bin-tail")) {
            ESP_LOGW(TAG, "terminal uplink tail failed: turn=%u terminal_gen=%u tail=%u; ending turn",
                     (unsigned)s_turn_id, (unsigned)terminal_generation, (unsigned)index);
            close_channel(false);
            return false;
        }
        log_uplink_traffic(s_terminal_tail_lengths[index]);
    }
    s_terminal_tail_count = 0U;
    s_uplink_terminal_listen_stop_started = true;
    s_uplink_tx_operation = "listen-stop";
    const bool stopped = send_listen_stop();
    ESP_LOGI(TAG, "terminal uplink op=listen-stop result=%d turn=%u terminal_gen=%u",
             stopped ? 1 : 0, (unsigned)s_turn_id, (unsigned)terminal_generation);
    s_uplink_tx_operation = "idle";
    if (!stopped) {
        ESP_LOGW(TAG, "terminal listen/stop failed: turn=%u terminal_gen=%u; ending turn",
                 (unsigned)s_turn_id, (unsigned)terminal_generation);
        close_channel(false);
        return false;
    }
    xiaozhi_audio_pipeline_reset_wake_word();
    reset_uplink_pacing();
    s_waiting_turn_id = s_turn_id;
    s_playing_turn_id = 0U;
    s_listen_start_sent_for_turn = false;
    s_response_deadline_us = esp_timer_get_time() + XIAOZHI_WAIT_RESPONSE_TIMEOUT_US;
    s_state = XIAOZHI_VOICE_WAIT_RESPONSE;
    ESP_LOGI(TAG, "voice VAD: speech ended; waiting for cloud response turn=%u terminal_gen=%u",
             (unsigned)s_waiting_turn_id, (unsigned)terminal_generation);
    publish_ui_state(VOICE_UI_THINKING);
    return true;
}

static void finish_listen_with_tail_flush(void)
{
    if (s_state != XIAOZHI_VOICE_LISTENING) return;
    (void)xiaozhi_audio_pipeline_set_streaming(false);
    s_uplink_failed = false;
    s_uplink_tail_deadline_us = esp_timer_get_time() + XIAOZHI_UPLINK_TAIL_FLUSH_TIMEOUT_US;
    s_state = XIAOZHI_VOICE_STOPPING;
    begin_terminal_uplink_transition();
    notify_uplink_task();
    log_uplink_queue_depth("terminal-cutoff");
    ESP_LOGI(TAG, "voice VAD silence; terminal tail frames<=%u timeout_ms=%u turn=%u terminal_gen=%u",
             (unsigned)XIAOZHI_UPLINK_TAIL_FLUSH_FRAMES,
             (unsigned)(XIAOZHI_UPLINK_TAIL_FLUSH_TIMEOUT_US / 1000LL),
             (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation);
    if (!wait_for_main_uplink_write()) {
        ESP_LOGW(TAG, "terminal main write wait timed out: turn=%u terminal_gen=%u in_flight=%d wait_us=%lld send_result=%d connected=%d",
                 (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation,
                 s_uplink_bin_in_flight ? 1 : 0, (long long)s_uplink_main_wait_us,
                 s_last_uplink_send_result,
                 s_last_uplink_send_connected ? 1 : 0);
        close_channel(false);
        return;
    }
    if (s_uplink_failed || s_last_uplink_send_result <= 0 || !capture_terminal_uplink_tails()) {
        ESP_LOGW(TAG, "terminal main write failed: turn=%u terminal_gen=%u in_flight=%d send_result=%d connected=%d; ending turn",
                 (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation,
                 s_uplink_bin_in_flight ? 1 : 0, s_last_uplink_send_result,
                 s_last_uplink_send_connected ? 1 : 0);
        close_channel(false);
        return;
    }
    (void)send_terminal_uplink_sequence();
}

static void send_detect(bool start_current_speech)
{
    if (!send_text("{\"type\":\"listen\",\"state\":\"detect\",\"text\":\"你好,小智\"}")) return;
    ++s_turn_id;
    if (s_turn_id == 0U) ++s_turn_id;
    s_greeting_turn_id = s_turn_id;
    s_waiting_turn_id = 0U;
    s_playing_turn_id = 0U;
    s_state = XIAOZHI_VOICE_WAIT_SPEECH;
    ESP_LOGI(TAG, "voice wake accepted; waiting for speech greeting_turn=%u",
             (unsigned)s_greeting_turn_id);
    publish_ui_state(VOICE_UI_LISTENING);
    if (start_current_speech) start_listen(true);
}

/* A follow-up already belongs to the established cloud session.  Sending a
 * second detect frame would make the fixed wake phrase look like user input. */
static void start_followup_listen(void)
{
    s_state = XIAOZHI_VOICE_WAIT_SPEECH;
    start_listen(true);
}

static void finish_wake_greeting_after_playback(void)
{
    if (s_waiting_for_wake_primer) {
        s_state = XIAOZHI_VOICE_WAIT_SPEECH;
    } else if (s_wake_primer_candidate_speech || s_vad_speech) {
        s_wake_primer_candidate_speech = false;
        s_wait_for_turn_silence = false;
        s_state = XIAOZHI_VOICE_WAIT_SPEECH;
        start_listen(true);
    } else if (!s_listen_start_sent_for_turn) {
        s_state = XIAOZHI_VOICE_WAIT_SPEECH;
        start_listen(false);
    } else {
        /* The primer completed while the wake greeting was audible. The
         * server's ignore window is already consumed, so direct follow-up is
         * safe without waiting for an unrelated future VAD_END. */
        s_wait_for_turn_silence = false;
        s_state = XIAOZHI_VOICE_WAIT_FOLLOWUP;
        s_followup_deadline_us = esp_timer_get_time() + XIAOZHI_FOLLOWUP_WINDOW_US;
    }
}

static void enter_followup_after_playback(void)
{
    const bool boot_greeting_finished = s_playing_boot_greeting;
    const bool wake_greeting_finished = s_playing_wake_greeting;
    const bool boot_greeting_window_active = boot_greeting_finished ||
                                             s_boot_greeting_direct_window_active;
    s_playing_turn_id = 0U;
    s_playing_boot_greeting = false;
    s_playing_wake_greeting = false;
    s_discard_tts_stream = false;
    s_tts_playback_epoch = 0U;
    s_vad_speech = false;
    s_wait_for_turn_silence = true;
    if (boot_greeting_finished) {
        s_boot_greeting_direct_window_active = true;
        s_boot_greeting_reply_deadline_us = esp_timer_get_time() +
                                            XIAOZHI_BOOT_GREETING_REPLY_WINDOW_US;
    }
    if (boot_greeting_window_active && s_boot_greeting_reply_deadline_us != 0) {
        s_state = XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY;
        s_ui_idle_deadline_us = 0;
        if (boot_greeting_finished) {
            ESP_LOGI(TAG, "boot greeting PCM drained; direct reply window=3min");
        } else {
            ESP_LOGI(TAG, "boot greeting direct reply window remains active after response TTS");
            ESP_LOGI(TAG, "boot greeting direct window kept after response TTS");
        }
    } else {
        s_state = XIAOZHI_VOICE_WAIT_FOLLOWUP;
        s_followup_deadline_us = esp_timer_get_time() + XIAOZHI_FOLLOWUP_WINDOW_US;
        s_ui_idle_deadline_us = 0;
        ESP_LOGI(TAG, "cloud TTS PCM drained; fresh VAD baseline, follow-up window=5s");
    }
    ESP_LOGI(TAG, "post-playback silence gate armed");
    /* Keep the established WSS/session for the next turn. A follow-up is
     * opened only when the original connection is still healthy; a dead
     * connection ends this turn instead of replaying audio on a new session. */
    const bool wss_ready = s_client != NULL && s_hello_ready &&
                           esp_websocket_client_is_connected(s_client);
    if (!wss_ready) {
        ESP_LOGW(TAG, "follow-up WSS health check failed after PCM drain; ending turn");
        xiaozhi_audio_pipeline_reset_after_transport_failure();
        close_channel(false);
        return;
    }
    xiaozhi_audio_pipeline_resume_after_playback();
    xiaozhi_audio_pipeline_arm_followup();
    if (wake_greeting_finished && !boot_greeting_window_active) {
        finish_wake_greeting_after_playback();
    }
    publish_ui_state(VOICE_UI_LISTENING);
}

static esp_err_t start_wss(bool foreground_session)
{
    if (s_client != NULL && s_hello_ready && esp_websocket_client_is_connected(s_client)) {
        if (foreground_session) {
            s_session_active = true;
            publish_ui_state(VOICE_UI_CONNECTING);
        }
        return ESP_OK;
    }
    if (s_client != NULL && s_client_started) {
        if (esp_websocket_client_is_connected(s_client)) {
            if (foreground_session) {
                s_session_active = true;
                publish_ui_state(VOICE_UI_CONNECTING);
            }
            return ESP_OK;
        }
        ESP_LOGW(TAG, "replacing stale started-but-disconnected WSS client: seq=%u",
                 (unsigned)s_wss_connection_seq);
        (void)esp_websocket_client_stop(s_client);
        (void)esp_websocket_client_destroy(s_client);
        s_client = NULL;
        s_client_started = false;
        s_hello_ready = false;
        s_session_id[0] = '\0';
    }
    if (s_client != NULL) close_channel(false);

    const char *url = xiaozhi_activation_service_get_websocket_url();
    const char *token = xiaozhi_activation_service_get_websocket_token();
    const int authorization_len = snprintf(s_authorization, sizeof(s_authorization), "Bearer %s", token);
    if (url[0] == '\0' || token[0] == '\0' || authorization_len < 0 ||
        (size_t)authorization_len >= sizeof(s_authorization)) {
        xiaozhi_activation_service_refresh_credentials();
        return ESP_ERR_INVALID_STATE;
    }
    const esp_websocket_client_config_t config = {
        .uri = url,
        .transport = WEBSOCKET_TRANSPORT_OVER_SSL,
        .disable_auto_reconnect = true,
        .task_core_id_set = true,
        .task_core_id = 0,
        .task_prio = 3,
        .task_stack = 4096,
        .buffer_size = 1024,
        .reconnect_timeout_ms = 2000,
        .network_timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    ++s_wss_connection_seq;
    if (s_wss_connection_seq == 0U) ++s_wss_connection_seq;
    ESP_LOGI(TAG, "WSS diagnostic build: transport-task-boundary-v1");
    ESP_LOGI(TAG, "WSS connection start: seq=%u url=%s", (unsigned)s_wss_connection_seq, url);
    ESP_LOGI(TAG, "WSS dynamic RX/TX buffers enabled; buffers allocate after task creation");
    ESP_LOGI(TAG, "WSS client allocation begin: internal=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    s_client = esp_websocket_client_init(&config);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "WSS client create failed: internal=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "WSS client allocation complete: internal=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "WSS header Authorization: before");
    esp_err_t header_err = esp_websocket_client_append_header(s_client, "Authorization", s_authorization);
    ESP_LOGI(TAG, "WSS header Authorization: after err=%d internal=%u largest=%u",
             (int)header_err, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "WSS header Protocol-Version: before");
    header_err = esp_websocket_client_append_header(s_client, "Protocol-Version", "1");
    ESP_LOGI(TAG, "WSS header Protocol-Version: after err=%d internal=%u largest=%u",
             (int)header_err, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "WSS header Client-Id: before");
    header_err = esp_websocket_client_append_header(s_client, "Client-Id", xiaozhi_activation_service_get_client_id());
    ESP_LOGI(TAG, "WSS header Client-Id: after err=%d internal=%u largest=%u",
             (int)header_err, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "WSS header Device-Id: before");
    header_err = esp_websocket_client_append_header(s_client, "Device-Id", xiaozhi_activation_service_get_device_id());
    ESP_LOGI(TAG, "WSS header Device-Id: after err=%d internal=%u largest=%u",
             (int)header_err, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "WSS register events: before");
    esp_err_t register_err = esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, websocket_event,
                                                           (void *)(uintptr_t)s_wss_connection_seq);
    ESP_LOGI(TAG, "WSS register events: after err=%d internal=%u largest=%u",
             (int)register_err, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    s_hello_ready = false;
    s_session_active = foreground_session;
    s_state = XIAOZHI_VOICE_WAIT_HELLO;
    if (foreground_session) publish_ui_state(VOICE_UI_CONNECTING);
    s_hello_deadline_us = esp_timer_get_time() + XIAOZHI_HELLO_TIMEOUT_US;
    ESP_LOGI(TAG, "WSS start memory: stack=%u internal=%u largest=%u",
             4096U,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (esp_websocket_client_start(s_client) != ESP_OK) {
        ESP_LOGE(TAG, "WSS task/transport start failed: internal=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        // A transport/TLS startup failure is transient; keep valid activation credentials.
        close_channel(false);
        return ESP_FAIL;
    }
    s_client_started = true;
    return ESP_OK;
}

static void prewarm_wss(void)
{
    if (!xiaozhi_activation_service_is_activated() || s_client != NULL) return;
    ESP_LOGI(TAG, "prewarming XiaoZhi WSS before wake-word memory fragmentation");
    if (start_wss(false) != ESP_OK) {
        s_prewarm_retry_us = esp_timer_get_time() + XIAOZHI_PREWARM_RETRY_US;
    }
}

static void begin_channel(void)
{
    if (!xiaozhi_activation_service_is_activated()) {
        ESP_LOGW(TAG, "wake ignored: XiaoZhi activation/token unavailable");
        xiaozhi_audio_pipeline_reset_wake_word();
        return;
    }
    s_session_active = true;
    s_pending_wake = true;
    s_wait_for_turn_silence = true;
    s_followup_deadline_us = 0;
    publish_ui_state(VOICE_UI_CONNECTING);
    if (s_client != NULL && s_hello_ready && esp_websocket_client_is_connected(s_client)) {
        send_detect(false);
        s_pending_wake = false;
        return;
    }
    if (start_wss(true) != ESP_OK) {
        s_pending_wake = false;
        ESP_LOGW(TAG, "foreground WSS start failed; showing voice connection error");
        close_channel(false);
    }
}

static void process_text(const uint8_t *data, size_t data_len)
{
    char message[XIAOZHI_TEXT_MAX_BYTES + 1U];
    if (data_len > XIAOZHI_TEXT_MAX_BYTES) return;
    memcpy(message, data, data_len);
    message[data_len] = '\0';
    ESP_LOGI(TAG, "cloud JSON received: %s", message);
    cJSON *root = cJSON_Parse(message);
    cJSON *type = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, "type") : NULL;
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        cJSON_Delete(root);
        return;
    }
    cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const char *state_text = cJSON_IsString(state) && state->valuestring != NULL ?
                                 state->valuestring : "-";
    if (strcmp(type->valuestring, "stt") == 0) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (s_state == XIAOZHI_VOICE_WAIT_RESPONSE && cJSON_IsString(text) &&
            text->valuestring != NULL) {
            s_response_deadline_us = esp_timer_get_time() + XIAOZHI_WAIT_RESPONSE_TIMEOUT_US;
            ESP_LOGI(TAG, "cloud STT received; response timeout refreshed turn=%u",
                     (unsigned)s_waiting_turn_id);
            const voice_iot_command_result_t result =
                voice_iot_command_service_execute(text->valuestring);
            if (result != VOICE_IOT_COMMAND_NOT_HANDLED) {
                ESP_LOGI(TAG, "local IoT command handled: result=%d text=%s", (int)result,
                         text->valuestring);
            }
        }
    } else if (strcmp(type->valuestring, "hello") == 0) {
        s_hello_ready = true;
        s_hello_deadline_us = 0;
        const cJSON *session_id = cJSON_GetObjectItemCaseSensitive(root, "session_id");
        if (cJSON_IsString(session_id) && session_id->valuestring != NULL) {
            strncpy(s_session_id, session_id->valuestring, sizeof(s_session_id) - 1U);
            s_session_id[sizeof(s_session_id) - 1U] = '\0';
            ESP_LOGI(TAG, "WSS hello session_id saved: %s", s_session_id);
        } else {
            s_session_id[0] = '\0';
            ESP_LOGW(TAG, "WSS hello did not provide session_id");
        }
        if (s_pending_wake) {
            s_state = XIAOZHI_VOICE_WAIT_WAKE;
            send_detect(false);
            s_pending_wake = false;
        } else {
            s_state = XIAOZHI_VOICE_WAIT_WAKE;
            ESP_LOGI(TAG, "XiaoZhi WSS prewarm ready; waiting for wake word");
        }
    } else if (strcmp(type->valuestring, "tts") == 0) {
        if (cJSON_IsString(state) && strcmp(state->valuestring, "start") == 0) {
            const bool response_turn = s_state == XIAOZHI_VOICE_WAIT_RESPONSE &&
                                       s_waiting_turn_id != 0U;
            const bool greeting_turn = s_state == XIAOZHI_VOICE_WAIT_SPEECH &&
                                       s_greeting_turn_id != 0U;
            const bool boot_greeting_turn = s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_TTS &&
                                            s_boot_greeting_id != 0U;
            if (!response_turn && !greeting_turn && !boot_greeting_turn) {
                s_discard_tts_stream = true;
                ESP_LOGW(TAG, "duplicate/stale TTS start ignored: state=%d waiting_turn=%u playing_turn=%u",
                         s_state, (unsigned)s_waiting_turn_id, (unsigned)s_playing_turn_id);
                ESP_LOGI(TAG, "cloud JSON processed: type=%s state=%s",
                         type->valuestring, state_text);
                cJSON_Delete(root);
                return;
            }
            s_playing_turn_id = response_turn ? s_waiting_turn_id :
                                (greeting_turn ? s_greeting_turn_id : s_boot_greeting_id);
            s_waiting_turn_id = 0U;
            s_greeting_turn_id = 0U;
            s_boot_greeting_id = 0U;
            s_discard_tts_stream = false;
            s_playing_boot_greeting = boot_greeting_turn;
            s_playing_wake_greeting = greeting_turn;
            if (xiaozhi_audio_pipeline_begin_tts_playback(&s_tts_playback_epoch) != ESP_OK) {
                s_discard_tts_stream = true;
                s_playing_turn_id = 0U;
                s_playing_boot_greeting = false;
                s_playing_wake_greeting = false;
                s_state = XIAOZHI_VOICE_WAIT_WAKE;
                ESP_LOGW(TAG, "cloud TTS rejected: shared PCM playout unavailable");
                publish_ui_state(VOICE_UI_ERROR);
                schedule_ui_idle_after(XIAOZHI_UI_ERROR_HOLD_US);
                ESP_LOGI(TAG, "cloud JSON processed: type=%s state=%s",
                         type->valuestring, state_text);
                cJSON_Delete(root);
                return;
            }
            s_state = XIAOZHI_VOICE_TTS_BUFFERING;
            s_followup_deadline_us = 0;
            s_response_deadline_us = 0;
            if (response_turn && !s_boot_greeting_direct_window_active) {
                s_boot_greeting_reply_deadline_us = 0;
            }
            s_boot_greeting_tts_deadline_us = 0;
            s_tts_start_us = esp_timer_get_time();
            s_tts_last_packet_us = 0;
            s_tts_first_packet_seen = false;
            s_tts_first_pcm_seen = false;
            if (boot_greeting_turn) {
                ESP_LOGI(TAG, "boot greeting tts start received: id=%u", (unsigned)s_playing_turn_id);
            }
            ESP_LOGI(TAG, "cloud TTS accepted: turn=%u epoch=%u; waiting for PCM prebuffer",
                     (unsigned)s_playing_turn_id, (unsigned)s_tts_playback_epoch);
            if (greeting_turn) {
                ESP_LOGI(TAG, "wake greeting TTS accepted: turn=%u", (unsigned)s_playing_turn_id);
                if (!start_wake_server_ignore_primer()) {
                    s_discard_tts_stream = true;
                    s_playing_turn_id = 0U;
                    s_playing_wake_greeting = false;
                    close_channel(false);
                    ESP_LOGI(TAG, "cloud JSON processed: type=%s state=%s",
                             type->valuestring, state_text);
                    cJSON_Delete(root);
                    return;
                }
            }
            if (boot_greeting_turn) {
                ESP_LOGI(TAG, "boot greeting TTS accepted: id=%u", (unsigned)s_playing_turn_id);
            }
            publish_ui_state(VOICE_UI_THINKING);
        } else if (cJSON_IsString(state) && strcmp(state->valuestring, "stop") == 0) {
            if (s_discard_tts_stream) {
                s_discard_tts_stream = false;
                ESP_LOGI(TAG, "rejected TTS stream ended");
                ESP_LOGI(TAG, "cloud JSON processed: type=%s state=%s",
                         type->valuestring, state_text);
                cJSON_Delete(root);
                return;
            }
            if (s_state != XIAOZHI_VOICE_TTS_BUFFERING &&
                s_state != XIAOZHI_VOICE_SPEAKING) {
                ESP_LOGI(TAG, "stale cloud TTS stop ignored in state=%d", s_state);
                ESP_LOGI(TAG, "cloud JSON processed: type=%s state=%s",
                         type->valuestring, state_text);
                cJSON_Delete(root);
                return;
            }
            /* TTS stop ends the network stream. The shared playout task retains
             * decoded tail PCM and reports only after it is fully drained/muted. */
            xiaozhi_audio_pipeline_finish_playback();
            s_state = XIAOZHI_VOICE_TTS_DRAINING;
            ESP_LOGI(TAG, "cloud TTS stopped; waiting for PCM drain epoch=%u",
                     (unsigned)s_tts_playback_epoch);
        }
    } else if (strcmp(type->valuestring, "alert") == 0) {
        const cJSON *alert_message = cJSON_GetObjectItemCaseSensitive(root, "message");
        const char *detail = cJSON_IsString(alert_message) && alert_message->valuestring != NULL
                                 ? alert_message->valuestring : "cloud alert";
        ESP_LOGW(TAG, "cloud alert: %s", detail);
        if (s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_TTS) {
            finish_boot_greeting_failure(detail);
            ESP_LOGI(TAG, "boot greeting rejected by cloud; ordinary wake word remains available");
            ESP_LOGI(TAG, "cloud JSON processed: type=%s state=%s", type->valuestring, state_text);
            cJSON_Delete(root);
            return;
        }
    } else if (strcmp(type->valuestring, "error") == 0) {
        if (s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_TTS ||
            s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY || s_playing_boot_greeting) {
            finish_boot_greeting_failure("cloud error");
            ESP_LOGI(TAG, "cloud JSON processed: type=%s state=%s", type->valuestring, state_text);
            cJSON_Delete(root);
            return;
        }
        ESP_LOGW(TAG, "cloud returned an error frame; refreshing credentials");
        close_channel(true);
    }
    ESP_LOGI(TAG, "cloud JSON processed: type=%s state=%s", type->valuestring, state_text);
    cJSON_Delete(root);
}

static bool receive_uplink_frame(uint8_t *destination, size_t *length)
{
    if (destination == NULL || length == NULL || s_uplink_queue == NULL) return false;
    size_t item_size = 0U;
    if (s_uplink_queue_lock != NULL) xSemaphoreTake(s_uplink_queue_lock, portMAX_DELAY);
    uint8_t *item = xRingbufferReceive(s_uplink_queue, &item_size, 0);
    if (item != NULL) {
        if (item_size <= XIAOZHI_MAX_OPUS_FRAME_BYTES) {
            memcpy(destination, item, item_size);
            *length = item_size;
        }
        vRingbufferReturnItem(s_uplink_queue, item);
    }
    if (s_uplink_queue_lock != NULL) xSemaphoreGive(s_uplink_queue_lock);
    return item != NULL && *length > 0U;
}

static void report_uplink_failed(void)
{
    if (s_uplink_failed) return;
    s_uplink_failed = true;
    s_uplink_enabled = false;
    s_uplink_primer_active = false;
    s_uplink_primer_frames_remaining = 0U;
    notify_uplink_task();
    (void)post_event(VOICE_EVENT_UPLINK_FAILED);
}

static void voice_uplink_tx_task(void *arg)
{
    (void)arg;
    if (s_uplink_frame_buffer == NULL) {
        ESP_LOGE(TAG, "voice uplink TX buffer allocation failed");
        report_uplink_failed();
        vTaskSuspend(NULL);
        return;
    }
    for (;;) {
        if (!s_uplink_enabled && !s_uplink_primer_active) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }

        const bool primer = s_uplink_primer_active;

        if (s_next_uplink_deadline_us != 0) {
            const int64_t remaining_us = s_next_uplink_deadline_us - esp_timer_get_time();
            if (remaining_us > 0) {
                const uint32_t delay_ms = (uint32_t)(((remaining_us + 9999LL) / 10000LL) * 10U);
                (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms > 10U ? 10U : delay_ms));
                continue;
            }
        }

        /* Reserve the terminal boundary before touching the queue. If VAD_END
         * wins after this admission, the terminal owner waits for this one
         * already-admitted write instead of silently losing the dequeued tail. */
        if (!begin_main_uplink_send()) continue;
        size_t frame_len = 0U;
        if (!receive_uplink_frame(s_uplink_frame_buffer, &frame_len)) {
            (void)finish_main_uplink_send();
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
            continue;
        }
        if (frame_len > XIAOZHI_MAX_OPUS_FRAME_BYTES) {
            (void)finish_main_uplink_send();
            continue;
        }
        if (!send_uplink_packet(s_uplink_frame_buffer, frame_len, "bin-main")) {
            const bool terminal_deadline = finish_main_uplink_send();
            if (terminal_deadline || s_uplink_terminal_active) {
                s_uplink_failed = true;
                ESP_LOGW(TAG, "terminal main write failed in terminal path: turn=%u terminal_gen=%u deadline=%d; terminal owner will end turn",
                         (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation,
                         terminal_deadline ? 1 : 0);
            } else {
                report_uplink_failed();
            }
            if (terminal_deadline) {
                ESP_LOGW(TAG, "terminal main write completed after deadline: turn=%u terminal_gen=%u; ending turn safely",
                         (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation);
            }
            continue;
        }
        const bool terminal_deadline = finish_main_uplink_send();
        if (terminal_deadline) {
            ESP_LOGW(TAG, "terminal main write completed after deadline: turn=%u terminal_gen=%u; ending turn safely",
                     (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation);
            continue;
        }
        advance_uplink_pacing();
        log_uplink_traffic(frame_len);
        if (primer && s_uplink_primer_frames_remaining > 0U) --s_uplink_primer_frames_remaining;
        if (primer && s_uplink_primer_frames_remaining == 0U) {
            s_uplink_primer_active = false;
            reset_uplink_pacing();
            (void)post_event(VOICE_EVENT_WAKE_PRIMER_DONE);
            xTaskNotifyGive(s_session_task);
        }
    }
}

static void handle_event(voice_event_t *event)
{
    switch (event->type) {
    case VOICE_EVENT_WAKE:
        if (s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_TTS ||
            s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY || s_playing_boot_greeting) {
            ESP_LOGI(TAG, "wake word ignored while boot greeting owns the direct-reply session");
            break;
        }
        if (s_state == XIAOZHI_VOICE_TTS_BUFFERING ||
            s_state == XIAOZHI_VOICE_SPEAKING ||
            s_state == XIAOZHI_VOICE_TTS_DRAINING) {
            (void)send_text("{\"type\":\"abort\",\"reason\":\"wake_word_detected\"}");
            ESP_LOGI(TAG, "voice turn abort: id=%u", (unsigned)s_playing_turn_id);
            s_playing_turn_id = 0U;
            s_waiting_turn_id = 0U;
            s_discard_tts_stream = false;
            s_playing_boot_greeting = false;
            xiaozhi_audio_pipeline_abort_playback();
        }
        if (s_state == XIAOZHI_VOICE_WAIT_HELLO) s_pending_wake = true;
        else begin_channel();
        break;
    case VOICE_EVENT_VAD_START:
        s_vad_speech = true;
        if (s_waiting_for_wake_primer) {
            s_wake_primer_candidate_speech = true;
            ESP_LOGI(TAG, "wake primer candidate speech observed; waiting for primer done");
            break;
        }
        if (s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY) {
            if (s_wait_for_turn_silence) {
                ESP_LOGI(TAG, "boot greeting VAD start ignored; waiting for silence");
                break;
            }
            s_state = XIAOZHI_VOICE_WAIT_SPEECH;
            start_listen(true);
            ESP_LOGI(TAG, "boot greeting direct reply detected; starting cloud turn");
        } else if (s_state == XIAOZHI_VOICE_WAIT_FOLLOWUP) {
            if (s_wait_for_turn_silence) {
                ESP_LOGI(TAG, "follow-up VAD start ignored; waiting for silence");
                break;
            }
            start_followup_listen();
            s_followup_deadline_us = 0;
            ESP_LOGI(TAG, "follow-up speech detected; starting next cloud turn");
        } else if (s_state == XIAOZHI_VOICE_WAIT_SPEECH && s_wait_for_turn_silence) {
            ESP_LOGI(TAG, "wake-word VAD start ignored; waiting for silence boundary");
        } else {
            start_listen(true);
        }
        break;
    case VOICE_EVENT_VAD_END:
        s_vad_speech = false;
        if (s_waiting_for_wake_primer) {
            s_wake_primer_candidate_speech = false;
            ESP_LOGI(TAG, "wake primer candidate speech cleared by VAD end");
            break;
        }
        if (s_state == XIAOZHI_VOICE_TTS_BUFFERING ||
            s_state == XIAOZHI_VOICE_SPEAKING ||
            s_state == XIAOZHI_VOICE_TTS_DRAINING) {
            ESP_LOGI(TAG, "wake-word VAD end ignored while TTS is speaking");
            break;
        }
        if (s_wait_for_turn_silence) {
            s_wait_for_turn_silence = false;
            xiaozhi_audio_pipeline_reset_after_silence();
            ESP_LOGI(TAG, "post-playback silence observed; ready for question");
            ESP_LOGI(TAG, "voice turn silence boundary observed; ready for question");
            break;
        }
        if (s_state == XIAOZHI_VOICE_LISTENING) finish_listen_with_tail_flush();
        break;
    case VOICE_EVENT_NETWORK_LOST:
        close_channel(false);
        break;
    case VOICE_EVENT_WSS_CONNECTED:
        if (event->value != s_wss_connection_seq) {
            ESP_LOGI(TAG, "stale WSS connected event ignored: event_seq=%u current_seq=%u",
                     (unsigned)event->value, (unsigned)s_wss_connection_seq);
            break;
        }
        if (s_state == XIAOZHI_VOICE_WAIT_HELLO) {
            const char *hello = "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\",\"features\":{\"mcp\":true},\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}";
            if (!send_ws_text_raw(hello)) {
                ESP_LOGW(TAG, "WSS hello send failed; ending current connection");
                close_channel(false);
            }
        }
        break;
    case VOICE_EVENT_WSS_DISCONNECTED:
        if (event->value != s_wss_connection_seq) {
            ESP_LOGI(TAG, "stale WSS disconnect ignored: event_seq=%u current_seq=%u",
                     (unsigned)event->value, (unsigned)s_wss_connection_seq);
            break;
        }
        if (s_client == NULL && !s_session_active && !s_hello_ready &&
            s_state == XIAOZHI_VOICE_WAIT_WAKE) {
            ESP_LOGI(TAG, "duplicate WSS disconnect ignored: seq=%u idle already",
                     (unsigned)event->value);
            break;
        }
        ESP_LOGW(TAG, "WSS disconnected: seq=%u event_id=%ld state=%d turn=%u session=%s hello=%d active=%d disconnect_before_listen_stop=%d",
                 (unsigned)event->value, (long)(int32_t)event->detail, s_state, (unsigned)s_turn_id,
                 s_session_id[0] != '\0' ? s_session_id : "-", s_hello_ready ? 1 : 0,
                 s_session_active ? 1 : 0,
                 s_uplink_terminal_listen_stop_started ? 0 : 1);
        // A WSS disconnect ends only the current voice turn. Keep activation
        // credentials and let the normal idle prewarm establish a later session.
        xiaozhi_audio_pipeline_reset_after_transport_failure();
        close_channel(false);
        break;
    case VOICE_EVENT_WSS_TEXT:
        if (event->value != s_wss_connection_seq) {
            ESP_LOGI(TAG, "stale WSS text ignored: event_seq=%u current_seq=%u",
                     (unsigned)event->value, (unsigned)s_wss_connection_seq);
            break;
        }
        process_text(event->payload, event->payload_len);
        break;
    case VOICE_EVENT_WSS_BINARY:
        if (s_discard_tts_stream ||
            (s_state != XIAOZHI_VOICE_TTS_BUFFERING && s_state != XIAOZHI_VOICE_SPEAKING) ||
            event->value != s_wss_connection_seq ||
            s_playing_turn_id == 0U) {
            ESP_LOGW(TAG, "late TTS packet discarded outside accepted speaking turn: state=%d turn=%u bytes=%u event_seq=%u current_seq=%u",
                     s_state, (unsigned)s_playing_turn_id, (unsigned)event->payload_len,
                     (unsigned)event->value, (unsigned)s_wss_connection_seq);
            break;
        }
        {
            const int64_t now_us = esp_timer_get_time();
            if (event->payload_len < 5U) {
                ++s_tts_short_packet_count;
                ESP_LOGW(TAG, "TTS short binary packet: turn=%u bytes=%u count=%u",
                         (unsigned)s_playing_turn_id, (unsigned)event->payload_len,
                         (unsigned)s_tts_short_packet_count);
            }
            if (!s_tts_first_packet_seen) {
                s_tts_first_packet_seen = true;
                ESP_LOGI(TAG, "tts first binary latency: turn=%u delay_us=%lld",
                         (unsigned)s_playing_turn_id, (long long)(now_us - s_tts_start_us));
            }
            if (s_tts_packet_log_us == 0 || now_us - s_tts_packet_log_us >= XIAOZHI_TRAFFIC_LOG_INTERVAL_US) {
                ESP_LOGI(TAG, "TTS packet arrival: turn=%u bytes=%u interval_us=%lld",
                         (unsigned)s_playing_turn_id, (unsigned)event->payload_len,
                         (long long)(s_tts_last_packet_us == 0 ? 0 : now_us - s_tts_last_packet_us));
                s_tts_packet_log_us = now_us;
            }
            s_tts_last_packet_us = now_us;
        }
        if (xiaozhi_audio_pipeline_enqueue_tts_opus(event->payload, event->payload_len) != ESP_OK) {
            ++s_tts_packet_drop_count;
            ESP_LOGW(TAG, "TTS packet dropped: playback queue/result error count=%u",
                     (unsigned)s_tts_packet_drop_count);
        } else {
            log_tts_traffic(event->payload_len);
        }
        break;
    case VOICE_EVENT_BOOT_GREETING:
        begin_boot_greeting();
        break;
    case VOICE_EVENT_TTS_PLAYBACK_STARTED:
        if (event->value != s_tts_playback_epoch ||
            (s_state != XIAOZHI_VOICE_TTS_BUFFERING && s_state != XIAOZHI_VOICE_TTS_DRAINING)) {
            ESP_LOGI(TAG, "stale TTS playout start ignored: epoch=%u state=%d",
                     (unsigned)event->value, s_state);
            break;
        }
        if (s_state == XIAOZHI_VOICE_TTS_BUFFERING) s_state = XIAOZHI_VOICE_SPEAKING;
        if (s_playing_boot_greeting && !s_tts_first_pcm_seen) {
            s_tts_first_pcm_seen = true;
            ESP_LOGI(TAG, "boot greeting first decodable PCM latency: id=%u delay_us=%lld",
                     (unsigned)s_playing_turn_id,
                     (long long)(esp_timer_get_time() - s_tts_start_us));
        }
        ESP_LOGI(TAG, "cloud TTS audible: turn=%u epoch=%u start_to_pcm_us=%lld",
                 (unsigned)s_playing_turn_id, (unsigned)event->value,
                 (long long)(esp_timer_get_time() - s_tts_start_us));
        publish_ui_state(VOICE_UI_SPEAKING);
        break;
    case VOICE_EVENT_TTS_PLAYBACK_FINISHED:
        if (event->value != s_tts_playback_epoch || s_state != XIAOZHI_VOICE_TTS_DRAINING) {
            ESP_LOGI(TAG, "stale TTS playout finish ignored: epoch=%u state=%d",
                     (unsigned)event->value, s_state);
            break;
        }
        enter_followup_after_playback();
        break;
    case VOICE_EVENT_TTS_PLAYBACK_UNDERRUN:
        if (event->value == s_tts_playback_epoch) {
            ESP_LOGW(TAG, "TTS playout underrun reported: turn=%u epoch=%u",
                     (unsigned)s_playing_turn_id, (unsigned)event->value);
        }
        break;
    case VOICE_EVENT_UPLINK_FAILED:
        if (s_state == XIAOZHI_VOICE_STOPPING) {
            ESP_LOGW(TAG, "terminal Opus transport failure: turn=%u terminal_gen=%u; ending turn",
                     (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation);
            close_channel(false);
        } else if (s_state == XIAOZHI_VOICE_LISTENING || s_waiting_for_wake_primer) {
            ESP_LOGW(TAG, "voice turn ended after Opus transport failure; wake word required");
            close_channel(false);
        }
        break;
    case VOICE_EVENT_WAKE_PRIMER_DONE:
        if (s_waiting_for_wake_primer) {
            s_waiting_for_wake_primer = false;
            s_wake_primer_deadline_us = 0;
            ESP_LOGI(TAG, "wake server ignore primer done: frames=%u turn=%u",
                     (unsigned)XIAOZHI_WAKE_PRIMER_FRAMES, (unsigned)s_turn_id);
            if (s_state == XIAOZHI_VOICE_TTS_BUFFERING ||
                s_state == XIAOZHI_VOICE_SPEAKING ||
                s_state == XIAOZHI_VOICE_TTS_DRAINING) {
                break;
            }
            if (s_wake_primer_candidate_speech || s_vad_speech) {
                s_wake_primer_candidate_speech = false;
                s_wait_for_turn_silence = false;
                start_listen(true);
            } else {
                s_wait_for_turn_silence = false;
                s_state = XIAOZHI_VOICE_WAIT_FOLLOWUP;
                s_followup_deadline_us = esp_timer_get_time() + XIAOZHI_FOLLOWUP_WINDOW_US;
            }
        }
        break;
    }
}

static void voice_session_task(void *arg)
{
    (void)arg;
    for (;;) {
        voice_event_t event = {0};
        if (xQueueReceive(s_event_queue, &event, pdMS_TO_TICKS(20)) == pdTRUE) {
            handle_event(&event);
            free_event(&event);
        }
        if (s_ui_idle_deadline_us != 0 && esp_timer_get_time() >= s_ui_idle_deadline_us) {
            s_ui_idle_deadline_us = 0;
            if (s_state == XIAOZHI_VOICE_WAIT_WAKE) {
                s_session_active = false;
                publish_ui_state(VOICE_UI_IDLE);
                if (s_client == NULL && xiaozhi_activation_service_is_activated()) {
                    s_prewarm_retry_us = esp_timer_get_time() + XIAOZHI_PREWARM_RETRY_US;
                }
            }
        }
        if (s_state == XIAOZHI_VOICE_WAIT_HELLO && s_hello_deadline_us != 0 &&
            esp_timer_get_time() >= s_hello_deadline_us) {
            ESP_LOGW(TAG, "cloud hello timeout");
            close_channel(false);
        }
        if (s_waiting_for_wake_primer && s_wake_primer_deadline_us != 0 &&
            esp_timer_get_time() >= s_wake_primer_deadline_us) {
            ESP_LOGW(TAG, "wake server ignore primer timeout; wake word required");
            close_channel(false);
        }
        if (s_state == XIAOZHI_VOICE_WAIT_RESPONSE && s_response_deadline_us != 0 &&
            esp_timer_get_time() >= s_response_deadline_us) {
            s_response_deadline_us = 0;
            ESP_LOGW(TAG, "cloud response timeout; ending voice turn");
            close_channel(false);
        }
        if (s_state == XIAOZHI_VOICE_WAIT_FOLLOWUP && s_followup_deadline_us != 0 &&
            esp_timer_get_time() >= s_followup_deadline_us) {
            s_followup_deadline_us = 0;
            s_wait_for_turn_silence = false;
            s_state = XIAOZHI_VOICE_WAIT_WAKE;
            s_session_active = false;
            xiaozhi_audio_pipeline_reset_wake_word();
            ESP_LOGI(TAG, "follow-up window expired; wake word required");
            publish_ui_state(VOICE_UI_IDLE);
        }
        if (s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_TTS &&
            s_boot_greeting_tts_deadline_us != 0 &&
            esp_timer_get_time() >= s_boot_greeting_tts_deadline_us) {
            ESP_LOGW(TAG, "boot greeting TTS timeout");
            finish_boot_greeting_failure("TTS start timeout");
        }
        if (s_boot_greeting_reply_deadline_us != 0 && s_session_active &&
            (s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY ||
             s_state == XIAOZHI_VOICE_WAIT_SPEECH ||
             s_state == XIAOZHI_VOICE_LISTENING ||
             s_state == XIAOZHI_VOICE_WAIT_RESPONSE ||
             s_state == XIAOZHI_VOICE_WAIT_HELLO) &&
            esp_timer_get_time() >= s_boot_greeting_reply_deadline_us) {
            s_boot_greeting_reply_deadline_us = 0;
            s_boot_greeting_direct_window_active = false;
            s_wait_for_turn_silence = false;
            xiaozhi_audio_pipeline_reset_after_transport_failure();
            if (s_client != NULL && !s_hello_ready) {
                if (s_client_started) (void)esp_websocket_client_stop(s_client);
                (void)esp_websocket_client_destroy(s_client);
                s_client = NULL;
                s_client_started = false;
            }
            s_state = XIAOZHI_VOICE_WAIT_WAKE;
            s_session_active = false;
            ESP_LOGI(TAG, "boot greeting reply window expired; wake word required");
            publish_ui_state(VOICE_UI_IDLE);
        }
        if (s_prewarm_retry_us != 0 && esp_timer_get_time() >= s_prewarm_retry_us &&
            s_client == NULL && !s_session_active) {
            s_prewarm_retry_us = 0;
            prewarm_wss();
        }
    }
}

static bool wait_for_prewarmed_wss(void)
{
    const int64_t deadline_us = esp_timer_get_time() + XIAOZHI_PREWARM_READY_TIMEOUT_US;
    while (!s_hello_ready && esp_timer_get_time() < deadline_us) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_hello_ready) {
        ESP_LOGI(TAG, "WSS prewarm ready before AFE allocation");
        return true;
    }
    ESP_LOGW(TAG, "WSS prewarm handshake timed out before AFE allocation (client=%p started=%d)",
             (void *)s_client, s_client_started ? 1 : 0);
    return false;
}

static void websocket_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    const uint32_t connection_seq = (uint32_t)(uintptr_t)handler_args;
    (void)base;
    ESP_LOGI(TAG, "WSS callback event_id=%ld connection_seq=%u op=%s terminal_gen=%u terminal=%d",
             (long)event_id, (unsigned)connection_seq, s_uplink_tx_operation,
             (unsigned)s_uplink_terminal_generation, s_uplink_terminal_active ? 1 : 0);
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        post_event_value(VOICE_EVENT_WSS_CONNECTED, connection_seq);
    } else if (event_id == WEBSOCKET_EVENT_DATA) {
        receive_websocket_payload((const esp_websocket_event_data_t *)event_data, connection_seq);
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_ERROR ||
               event_id == WEBSOCKET_EVENT_CLOSED) {
        ESP_LOGW(TAG, "WSS disconnect callback kind=%s event_id=%ld connection_seq=%u op=%s terminal_gen=%u",
                 wss_event_name(event_id), (long)event_id, (unsigned)connection_seq, s_uplink_tx_operation,
                 (unsigned)s_uplink_terminal_generation);
        post_wss_disconnect_event(connection_seq, event_id);
    }
}

static void wake_word_detected(void *context)
{
    (void)context;
    ESP_LOGI(TAG, "WakeNet detected; queuing voice session");
    post_event(VOICE_EVENT_WAKE);
}

static void vad_changed(bool speech, void *context)
{
    (void)context;
    post_event(speech ? VOICE_EVENT_VAD_START : VOICE_EVENT_VAD_END);
}

static void tts_playback_changed(xiaozhi_tts_playback_event_t event, uint32_t epoch, void *context)
{
    (void)context;
    switch (event) {
    case XIAOZHI_TTS_PLAYBACK_STARTED:
        post_playback_event(VOICE_EVENT_TTS_PLAYBACK_STARTED, epoch);
        break;
    case XIAOZHI_TTS_PLAYBACK_FINISHED:
        post_playback_event(VOICE_EVENT_TTS_PLAYBACK_FINISHED, epoch);
        break;
    case XIAOZHI_TTS_PLAYBACK_UNDERRUN:
        post_playback_event(VOICE_EVENT_TTS_PLAYBACK_UNDERRUN, epoch);
        break;
    }
}

static void voice_pipeline_init_task(void *arg)
{
    TaskHandle_t waiter = (TaskHandle_t)arg;
    s_pipeline_init_result = xiaozhi_audio_pipeline_init(wake_word_detected, vad_changed,
                                                         opus_frame_ready, tts_playback_changed,
                                                         NULL);
    const unsigned watermark = (unsigned)uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "voice pipeline init task result=%s", esp_err_to_name(s_pipeline_init_result));
    ESP_LOGI(TAG, "voice pipeline init internal stack watermark=%u", watermark);
    s_pipeline_init_task = NULL;
    if (waiter != NULL) xTaskNotifyGive(waiter);
    vTaskDelete(NULL);
}

static esp_err_t init_audio_pipeline_on_internal_stack(void)
{
    s_pipeline_init_result = ESP_ERR_INVALID_STATE;
    s_pipeline_init_waiter = xTaskGetCurrentTaskHandle();
    ESP_LOGI(TAG, "voice pipeline init task create: static stack=%u tcb=%u internal=%u largest=%u",
             (unsigned)XIAOZHI_PIPELINE_INIT_STACK_BYTES,
             (unsigned)sizeof(s_pipeline_init_tcb),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    s_pipeline_init_task = xTaskCreateStaticPinnedToCore(
        voice_pipeline_init_task, "voice_pipeline_init", XIAOZHI_PIPELINE_INIT_STACK_BYTES,
        s_pipeline_init_waiter, 2, s_pipeline_init_stack, &s_pipeline_init_tcb, 1);
    if (s_pipeline_init_task == NULL) {
        s_pipeline_init_waiter = NULL;
        ESP_LOGW(TAG, "voice pipeline init task create failed: static internal stack=%u tcb=%u internal=%u largest=%u",
                 (unsigned)sizeof(s_pipeline_init_stack), (unsigned)sizeof(s_pipeline_init_tcb),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    s_pipeline_init_waiter = NULL;
    return s_pipeline_init_result;
}

static void opus_frame_ready(const uint8_t *opus, size_t opus_bytes, void *context)
{
    (void)context;
    if (opus == NULL || opus_bytes == 0U || s_uplink_queue == NULL) return;
    if (s_uplink_queue_lock != NULL) xSemaphoreTake(s_uplink_queue_lock, portMAX_DELAY);
    if (s_uplink_terminal_active) {
        ++s_uplink_drop_count;
        if (s_uplink_queue_lock != NULL) xSemaphoreGive(s_uplink_queue_lock);
        ESP_LOGI(TAG, "Opus uplink rejected at terminal boundary: turn=%u terminal_gen=%u dropped=%u",
                 (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation,
                 (unsigned)s_uplink_drop_count);
        return;
    }
    BaseType_t sent = xRingbufferSend(s_uplink_queue, opus, opus_bytes, 0);
    if (sent != pdTRUE) {
        size_t dropped_size = 0U;
        void *dropped = xRingbufferReceive(s_uplink_queue, &dropped_size, 0);
        if (dropped != NULL) vRingbufferReturnItem(s_uplink_queue, dropped);
        sent = xRingbufferSend(s_uplink_queue, opus, opus_bytes, 0);
        ++s_uplink_drop_count;
        ESP_LOGW(TAG, "Opus uplink queue full; dropped oldest frame and retried result=%d count=%u",
                 (int)sent, (unsigned)s_uplink_drop_count);
    }
    if (s_uplink_queue_lock != NULL) xSemaphoreGive(s_uplink_queue_lock);
    if (sent == pdTRUE) notify_uplink_task();
}

esp_err_t xiaozhi_voice_service_init(void)
{
    if (s_initialized) return ESP_OK;
    s_event_queue = xQueueCreate(XIAOZHI_EVENT_QUEUE_DEPTH, sizeof(voice_event_t));
    s_uplink_queue = xRingbufferCreateWithCaps(XIAOZHI_UPLINK_QUEUE_BYTES, RINGBUF_TYPE_NOSPLIT,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_uplink_queue_lock = xSemaphoreCreateMutex();
    s_ws_tx_lock = xSemaphoreCreateMutex();
    if (s_event_queue == NULL || s_uplink_queue == NULL || s_uplink_queue_lock == NULL ||
        s_ws_tx_lock == NULL) {
        cleanup_init_failure();
        return ESP_ERR_NO_MEM;
    }
    s_uplink_frame_buffer = heap_caps_malloc(XIAOZHI_MAX_OPUS_FRAME_BYTES,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_uplink_frame_buffer == NULL) {
        cleanup_init_failure();
        return ESP_ERR_NO_MEM;
    }
    /* ESP-WebSocket creates a fixed 4 KiB internal-RAM task. Reserve it
     * before AFE/WakeNet allocates and fragments the remaining internal heap. */
    s_initialized = true;
    publish_ui_state(VOICE_UI_IDLE);
    if (xiaozhi_audio_pipeline_prepare_encoder() != ESP_OK) {
        cleanup_init_failure();
        return ESP_ERR_NO_MEM;
    }
    prewarm_wss();
    /* The WSS callback posts CONNECTED/DATA events into s_event_queue. Create
     * its consumer after the socket task exists but before waiting for hello. */
    if (xTaskCreatePinnedToCoreWithCaps(voice_session_task, "voice_session", 6144, NULL, 3,
                                        &s_session_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        cleanup_init_failure();
        return ESP_ERR_NO_MEM;
    }
    if (!wait_for_prewarmed_wss()) {
        cleanup_init_failure();
        return ESP_ERR_TIMEOUT;
    }
    if (xTaskCreatePinnedToCoreWithCaps(voice_uplink_tx_task, "voice_uplink_tx", 6144, NULL, 4,
                                        &s_uplink_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        cleanup_init_failure();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = init_audio_pipeline_on_internal_stack();
    if (err != ESP_OK) {
        cleanup_init_failure();
        return err;
    }
    s_pipeline_ready = true;
    ESP_LOGI(TAG, "voice pipeline ready after internal-stack model init");
    ESP_LOGI(TAG, "xiaozhi voice link build: single-session-manual-v1");
    ESP_LOGI(TAG, "voice pipeline ready: WakeNet, VAD, Opus uplink and WSS hello complete");
    return ESP_OK;
}

bool xiaozhi_voice_service_is_ready(void)
{
    return s_initialized && s_pipeline_ready && s_hello_ready;
}

esp_err_t xiaozhi_voice_service_start_boot_greeting(void)
{
    if (!s_initialized || !s_pipeline_ready || !s_hello_ready || s_boot_greeting_attempted ||
        s_boot_greeting_request_pending) return ESP_ERR_INVALID_STATE;
    s_boot_greeting_request_pending = true;
    if (post_event(VOICE_EVENT_BOOT_GREETING)) return ESP_OK;
    s_boot_greeting_request_pending = false;
    return ESP_ERR_TIMEOUT;
}

void xiaozhi_voice_service_network_lost(void)
{
    if (!s_initialized) return;
    if (!s_session_active && s_state == XIAOZHI_VOICE_WAIT_WAKE && s_client == NULL) {
        ESP_LOGI(TAG, "voice network lost ignored: no active session");
        return;
    }
    post_event(VOICE_EVENT_NETWORK_LOST);
}
