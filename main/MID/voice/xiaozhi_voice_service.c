#include "xiaozhi_voice_service.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "xiaozhi_activation_service.h"
#include "xiaozhi_audio_pipeline.h"
#include "audio_playback_service.h"
#include "mid_snapshot.h"
#include "timer_chime_audio.h"
#include "timer_service.h"
#include "voice_iot_command_service.h"
#include "radio_stream_service.h"

#define XIAOZHI_EVENT_QUEUE_DEPTH 12U
#define XIAOZHI_UPLINK_QUEUE_BYTES (64U * 1024U)
#define XIAOZHI_TEXT_MAX_BYTES 1024U
#define XIAOZHI_BINARY_MAX_BYTES 2048U
#define XIAOZHI_HELLO_TIMEOUT_US (8LL * 1000LL * 1000LL)
#define XIAOZHI_PREWARM_READY_TIMEOUT_US (30LL * 1000LL * 1000LL)
#define XIAOZHI_UI_ERROR_HOLD_US (2LL * 1000LL * 1000LL)
#define XIAOZHI_PREWARM_RETRY_US (5LL * 1000LL * 1000LL)
#define XIAOZHI_PREWARM_RETRY_MAX_US (30LL * 1000LL * 1000LL)
#define XIAOZHI_TRAFFIC_LOG_INTERVAL_US (1LL * 1000LL * 1000LL)
#define XIAOZHI_OPUS_FRAME_INTERVAL_US (60LL * 1000LL)
#define XIAOZHI_UPLINK_TAIL_FLUSH_FRAMES 2U
#define XIAOZHI_UPLINK_TAIL_FLUSH_TIMEOUT_US (150LL * 1000LL)
#define XIAOZHI_UPLINK_MAIN_WRITE_WAIT_TIMEOUT_US (1200LL * 1000LL)
#define XIAOZHI_WAKE_PRIMER_FRAMES 35U
#define XIAOZHI_WAKE_PRIMER_TIMEOUT_US (10LL * 1000LL * 1000LL)
#define XIAOZHI_FOLLOWUP_WINDOW_US (15LL * 1000LL * 1000LL)
/* The vicp.fun tunnel server can exceed 10 s (PONG ages of 20-40 s were
 * observed in the field); 15 s avoids false "didn't catch that" timeouts
 * while the cloud ASR is merely slow. */
#define XIAOZHI_WAIT_RESPONSE_TIMEOUT_US (15LL * 1000LL * 1000LL)
#define XIAOZHI_EXIT_TTS_START_TIMEOUT_US (20LL * 1000LL * 1000LL)
#define XIAOZHI_EXIT_TTS_FIRST_PACKET_TIMEOUT_US (8LL * 1000LL * 1000LL)
#define XIAOZHI_EXIT_TTS_PLAYBACK_TIMEOUT_US (45LL * 1000LL * 1000LL)
/* Watchdog for the local error tone that answers a failed turn before the
 * session closes; the tone itself is 2 s, so 5 s leaves ample slack. */
#define XIAOZHI_ERROR_TONE_DEADLINE_US (5LL * 1000LL * 1000LL)
#define XIAOZHI_BOOT_GREETING_TTS_TIMEOUT_US (8LL * 1000LL * 1000LL)
#define XIAOZHI_BOOT_GREETING_REPLY_WINDOW_US (15LL * 1000LL * 1000LL)
/* After a playback underrun, if the server sends neither more TTS audio nor
 * a tts stop frame within this window, the stream is declared stalled and
 * playback finishes locally instead of hanging until the WSS heartbeat
 * timeout kills the whole session (server-side turn loss). */
#define XIAOZHI_TTS_STALL_TIMEOUT_US (16LL * 1000LL * 1000LL)
/* The post-TTS echo guard is bounded.  VAD starts below 500 are treated as
 * residual speaker audio; strong user speech bypasses it immediately. */
#define XIAOZHI_POST_PLAYBACK_SILENCE_GUARD_US (1500LL * 1000LL)
#define XIAOZHI_POST_PLAYBACK_SILENCE_RESET_PEAK 500U
#define XIAOZHI_DIRECT_REPLY_BYPASS_PEAK 1500U
/* While the boot greeting's 15 s direct-reply window is open, a VAD start
 * opens the cloud turn only when the captured peak is clearly speech.  The
 * mic sits on a shelf near the speaker, so residual echo / room noise often
 * reaches 500-1200 and would otherwise boot a spurious cloud turn with no
 * user input (observed peak 926-1210 during a silent window).  Requiring a
 * higher floor keeps ambient noise from opening a turn the user never asked
 * for, while clear close-mic speech still clears it. */
#define XIAOZHI_BOOT_GREETING_DIRECT_REPLY_MIN_PEAK 700U
#define XIAOZHI_CAPTURE_LOW_LEVEL_RMS 200U
#define XIAOZHI_MAX_OPUS_FRAME_BYTES 4096U
#define XIAOZHI_BOOT_GREETING_TEXT_MAX_BYTES 384U
#define XIAOZHI_SESSION_ID_MAX_BYTES 128U
#define XIAOZHI_PIPELINE_INIT_STACK_BYTES 4096U

typedef enum
{
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
    XIAOZHI_VOICE_EXITING,
    XIAOZHI_VOICE_BOOT_GREETING_WAIT_TTS,
    XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY,
    /* Local error tone is answering a failed turn; on FINISHED (or the
     * watchdog) the session closes and the main page returns. */
    XIAOZHI_VOICE_ERROR_TONE,
} xiaozhi_voice_state_t;

typedef enum
{
    VOICE_EVENT_WAKE = 0,
    VOICE_EVENT_VAD_START,
    VOICE_EVENT_VAD_END,
    VOICE_EVENT_CAPTURE_FINISHED,
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
    VOICE_EVENT_TIMER_RING,
    VOICE_EVENT_TIMER_STOPPED,
    VOICE_EVENT_ERROR_TONE_FINISHED,
} voice_event_type_t;

typedef struct
{
    voice_event_type_t type;
    uint8_t *payload;
    size_t payload_len;
    uint32_t value;
    uint32_t detail;
} voice_event_t;

typedef struct
{
    bool active;
    uint32_t connection_seq;
    uint8_t opcode;
    size_t length;
    size_t capacity;
    uint8_t *buffer;
} websocket_accumulator_t;

static const char *TAG = "xiaozhi_voice";
/* Timer chime epoch for the shared playout and the 1 s timer poll tick. */
static uint32_t s_chime_epoch;
static int64_t s_last_timer_tick_us;
/* Local error tone epoch and its closing watchdog deadline. */
static uint32_t s_error_tone_epoch;
static int64_t s_error_tone_deadline_us;
/* Set when "play radio" is acknowledged: the radio starts once the cloud
 * acknowledgement TTS finishes (see VOICE_EVENT_TTS_PLAYBACK_FINISHED). */
static bool s_pending_radio_play;
/* Set when the current cloud STT reserved a countdown.  It is consumed once
 * by TTS PCM drain, or by close_channel() when the voice chain fails first. */
static bool s_pending_countdown_start;
/* A local IoT reply (timer ack etc.) is deferred until the cloud reply for
 * the user's utterance has drained.  Sending it immediately opens a second
 * [device_call] TTS stream that races the cloud's reply stream on the shared
 * binary channel; the duplicate tts start then discards both streams' audio. */
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
/* Stop/destroy is owned by the session cleanup path.  Disconnect callbacks
 * only enqueue an event, but a transport failure and that event can still
 * converge while the client is stopping. */
static volatile bool s_client_cleanup_in_progress;
static bool s_connection_failed;
static bool s_hello_ready;
static bool s_vad_speech;
static bool s_wait_for_turn_silence;
static int64_t s_post_playback_silence_deadline_us;
static uint32_t s_capture_low_level_turn_id;
static uint32_t s_capture_low_level_rms;
static uint32_t s_capture_low_level_peak;
static bool s_pending_wake;
static bool s_session_active;
static int64_t s_hello_deadline_us;
static xiaozhi_voice_state_t s_state;
static int64_t s_ui_idle_deadline_us;
static int64_t s_prewarm_retry_us;
/* Backoff for the idle prewarm relaunch: starts at the base and doubles on
 * each DNS/prewarm failure up to the cap, so transient DNS jitter gets a few
 * retries before the voice path settles. Reset to base after a successful
 * start or when a foreground session begins. */
static int64_t s_prewarm_retry_delay_us;
static int64_t s_followup_deadline_us;
static int64_t s_response_deadline_us;
/* True while the local error tone is answering a failed turn (wake words are
 * ignored then, and close_channel returns to the idle UI without flashing the
 * error state); cleared on close_channel. */
static bool s_timeout_ui_active;
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
static const char *volatile s_uplink_tx_operation = "idle";
static uint8_t s_terminal_tail_frames[XIAOZHI_UPLINK_TAIL_FLUSH_FRAMES][XIAOZHI_MAX_OPUS_FRAME_BYTES];
static size_t s_terminal_tail_lengths[XIAOZHI_UPLINK_TAIL_FLUSH_FRAMES];
static uint8_t s_terminal_tail_count;
static uint32_t s_tts_frames;
static size_t s_tts_bytes;
static int64_t s_tts_log_us;
static int64_t s_tts_start_us;
static int64_t s_tts_last_packet_us;
static int64_t s_tts_stall_deadline_us;
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
static bool s_exit_prompt_pending;
static bool s_playing_exit_prompt;
static uint32_t s_exit_prompt_turn_id;
static bool s_timeout_prompt_pending;
static bool s_playing_timeout_prompt;
static uint32_t s_timeout_prompt_turn_id;
static bool s_fallback_prompt_pending;
static bool s_playing_fallback_prompt;
static uint32_t s_fallback_prompt_turn_id;
static int64_t s_exit_tts_start_deadline_us;
static int64_t s_exit_tts_first_packet_deadline_us;
static int64_t s_exit_tts_playback_deadline_us;
/* Each generated prompt gets at most one retry when its cloud TTS times out;
 * the flags reset when a new prompt of the same kind is requested. */
static bool s_exit_prompt_retried;
static bool s_timeout_prompt_retried;
static bool s_fallback_prompt_retried;
static uint32_t s_wss_connection_seq;
static int s_last_uplink_send_result;
static bool s_last_uplink_send_connected;
typedef enum
{
    XIAOZHI_WSS_HEALTH_CLOSED = 0,
    XIAOZHI_WSS_HEALTH_HEALTHY,
    XIAOZHI_WSS_HEALTH_STALE,
    XIAOZHI_WSS_HEALTH_FAILED,
} xiaozhi_wss_health_t;
static volatile xiaozhi_wss_health_t s_wss_health = XIAOZHI_WSS_HEALTH_CLOSED;
static volatile int64_t s_wss_last_ping_us;
static volatile int64_t s_wss_last_pong_us;
static volatile int64_t s_wss_last_rx_us;
static volatile int64_t s_wss_last_connected_us;
static volatile esp_websocket_error_codes_t s_wss_last_error;
static volatile int s_wss_last_close_code;

static void log_wss_wifi_state(void)
{
    wifi_ap_record_t ap = {0};
    const esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "WSS Wi-Fi state: connected=1 rssi=%d channel=%u", ap.rssi, ap.primary);
    }
    else
    {
        ESP_LOGI(TAG, "WSS Wi-Fi state: connected=0 err=%d", (int)err);
    }
}
static char s_session_id[XIAOZHI_SESSION_ID_MAX_BYTES];
static char s_authorization[600];
static websocket_accumulator_t s_rx;

/* 对话静默窗口到期后，从这里随机选择一条固定告别语发送给云端。 */
static const char *const s_exit_phrases[] = {
    "那我们今天先到这里啦，下次有需要随时找我～祝你一切顺利；",
    "本次对话结束，期待下次再见，愿你今天过得愉快；",
};

/* Spoken when the cloud turn times out.  The user can replace these with
 * their own wording; each entry is wrapped in [device_call] and sent to the
 * server, which answers with TTS that plays back locally. */
static const char *const s_timeout_phrases[] = {
    "抱歉，我刚才没有听清你说的话，可以再和我说一遍吗；",
    "刚刚好像没听清楚，你再说一次好吗；",
};

/* Spoken when a local turn fails (send failure, transport failure, playout
 * unavailable) so the user always hears an answer even on error paths.
 * Editable wording, same [device_call] mechanism as the timeout prompt. */
static const char *const s_fallback_phrases[] = {
    "抱歉，刚刚出了点小状况，我们再说一次吧；",
    "好像遇到了一点问题，没关系，再和我说一次；",
};

static void notify_uplink_task(void)
{
    if (s_uplink_task != NULL)
        xTaskNotifyGive(s_uplink_task);
}

static void websocket_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static esp_err_t start_wss(bool foreground_session);
static void opus_frame_ready(const uint8_t *opus, size_t opus_bytes, void *context);

static void publish_ui_state(voice_ui_state_t state)
{
    const voice_session_snapshot_t snapshot = {
        .initialized = s_initialized,
        .active = s_session_active,
        .connected = s_hello_ready && s_client != NULL &&
                     esp_websocket_client_is_connected(s_client),
        .connection_failed = s_connection_failed,
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
    if (event->payload != NULL)
        heap_caps_free(event->payload);
    event->payload = NULL;
    event->payload_len = 0;
}

static bool post_event(voice_event_type_t type)
{
    if (s_event_queue == NULL)
        return false;
    const voice_event_t event = {.type = type, .payload = NULL, .payload_len = 0, .value = 0U};
    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE)
    {
        ++s_voice_event_queue_full_count;
        ESP_LOGW(TAG, "voice event queue full: %d count=%u", type,
                 (unsigned)s_voice_event_queue_full_count);
        return false;
    }
    return true;
}

static bool post_event_detail(voice_event_type_t type, uint32_t value, uint32_t detail)
{
    if (s_event_queue == NULL)
        return false;
    const voice_event_t event = {.type = type, .payload = NULL, .payload_len = 0, .value = value, .detail = detail};
    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE)
    {
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
    if (event_id == WEBSOCKET_EVENT_ERROR)
        return "ERROR";
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED)
        return "DISCONNECTED";
    if (event_id == WEBSOCKET_EVENT_CLOSED)
        return "CLOSED";
    return "OTHER";
}

static void post_owned_event_value(voice_event_type_t type, uint8_t *payload, size_t payload_len,
                                   uint32_t value)
{
    if (payload == NULL || payload_len == 0U || s_event_queue == NULL)
    {
        heap_caps_free(payload);
        return;
    }
    const voice_event_t event = {.type = type, .payload = payload, .payload_len = payload_len, .value = value};
    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE)
    {
        ++s_voice_event_queue_full_count;
        ESP_LOGW(TAG, "voice event queue full: %d count=%u", type,
                 (unsigned)s_voice_event_queue_full_count);
        heap_caps_free(payload);
    }
}

static void post_playback_event(voice_event_type_t type, uint32_t epoch)
{
    if (s_event_queue == NULL)
        return;
    const voice_event_t event = {.type = type, .payload = NULL, .payload_len = 0, .value = epoch};
    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE)
    {
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
    s_client_cleanup_in_progress = true;
    s_uplink_enabled = false;
    s_uplink_terminal_active = true;
    s_uplink_terminal_close_pending = false;
    s_uplink_terminal_listen_stop_started = false;
    notify_uplink_task();
    if (s_uplink_task != NULL)
        vTaskDeleteWithCaps(s_uplink_task);
    s_uplink_task = NULL;
    if (s_pipeline_init_task != NULL)
        vTaskDelete(s_pipeline_init_task);
    s_pipeline_init_task = NULL;
    s_pipeline_init_waiter = NULL;
    heap_caps_free(s_uplink_frame_buffer);
    s_uplink_frame_buffer = NULL;
    xiaozhi_audio_pipeline_deinit();
    if (s_client != NULL)
    {
        if (s_client_started)
            (void)esp_websocket_client_stop(s_client);
        (void)esp_websocket_client_destroy(s_client);
        s_client = NULL;
        s_client_started = false;
    }
    if (s_session_task != NULL)
        vTaskDeleteWithCaps(s_session_task);
    if (s_event_queue != NULL)
        vQueueDelete(s_event_queue);
    if (s_uplink_queue != NULL)
        vRingbufferDeleteWithCaps(s_uplink_queue);
    if (s_uplink_queue_lock != NULL)
        vSemaphoreDelete(s_uplink_queue_lock);
    if (s_ws_tx_lock != NULL)
        vSemaphoreDelete(s_ws_tx_lock);
    rx_reset();
    s_event_queue = NULL;
    s_uplink_queue = NULL;
    s_uplink_queue_lock = NULL;
    s_ws_tx_lock = NULL;
    s_session_task = NULL;
    s_pipeline_ready = false;
    s_initialized = false;
    s_client_cleanup_in_progress = false;
}

static void receive_websocket_payload(const esp_websocket_event_data_t *event, uint32_t connection_seq)
{
    if (event == NULL)
        return;
    if (connection_seq != s_wss_connection_seq)
    {
        ESP_LOGI(TAG, "stale WSS data ignored: event_seq=%u current_seq=%u",
                 (unsigned)connection_seq, (unsigned)s_wss_connection_seq);
        return;
    }
    s_wss_last_rx_us = esp_timer_get_time();
    if (event->op_code == WS_TRANSPORT_OPCODES_PONG)
    {
        s_wss_last_pong_us = s_wss_last_rx_us;
        s_wss_health = XIAOZHI_WSS_HEALTH_HEALTHY;
        ESP_LOGI(TAG, "WSS pong received: seq=%u age_us=%lld", (unsigned)connection_seq,
                 (long long)(s_wss_last_rx_us - s_wss_last_connected_us));
        return;
    }
    if (event->data_ptr == NULL || event->data_len <= 0)
        return;
    const bool continuation = event->op_code == WS_TRANSPORT_OPCODES_CONT;
    if (!s_rx.active)
    {
        if (continuation)
        {
            ESP_LOGW(TAG, "orphan WSS continuation ignored");
            return;
        }
        const size_t capacity = event->op_code == WS_TRANSPORT_OPCODES_TEXT ? XIAOZHI_TEXT_MAX_BYTES : XIAOZHI_BINARY_MAX_BYTES;
        if (event->payload_len > (int)capacity || event->data_len > (int)capacity)
        {
            ESP_LOGW(TAG, "WSS payload exceeds cap: %d", event->payload_len);
            return;
        }
        s_rx.buffer = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_rx.buffer == NULL)
        {
            ESP_LOGW(TAG, "no PSRAM for WSS payload");
            return;
        }
        s_rx.active = true;
        s_rx.connection_seq = connection_seq;
        s_rx.opcode = event->op_code;
        s_rx.capacity = capacity;
    }
    else if (s_rx.connection_seq != connection_seq || (!continuation && event->payload_offset == 0))
    {
        rx_reset();
        receive_websocket_payload(event, connection_seq);
        return;
    }

    if ((size_t)event->data_len > s_rx.capacity - s_rx.length)
    {
        ESP_LOGW(TAG, "WSS reassembly overflow");
        rx_reset();
        return;
    }
    memcpy(s_rx.buffer + s_rx.length, event->data_ptr, (size_t)event->data_len);
    s_rx.length += (size_t)event->data_len;

    const bool end_of_payload = event->payload_len == 0 ||
                                ((size_t)event->payload_offset + (size_t)event->data_len >=
                                 (size_t)event->payload_len);
    if (!event->fin || !end_of_payload)
        return;

    const voice_event_type_t type = s_rx.opcode == WS_TRANSPORT_OPCODES_TEXT ? VOICE_EVENT_WSS_TEXT : VOICE_EVENT_WSS_BINARY;
    uint8_t *payload = s_rx.buffer;
    const size_t payload_len = s_rx.length;
    memset(&s_rx, 0, sizeof(s_rx));
    post_owned_event_value(type, payload, payload_len, connection_seq);
}

static uint32_t discard_uplink_queue(void)
{
    uint32_t discarded = 0U;
    if (s_uplink_queue == NULL)
        return discarded;
    if (s_uplink_queue_lock != NULL)
        xSemaphoreTake(s_uplink_queue_lock, portMAX_DELAY);
    for (;;)
    {
        size_t item_size = 0U;
        void *item = xRingbufferReceive(s_uplink_queue, &item_size, 0);
        if (item == NULL)
            break;
        vRingbufferReturnItem(s_uplink_queue, item);
        ++discarded;
    }
    if (s_uplink_queue_lock != NULL)
        xSemaphoreGive(s_uplink_queue_lock);
    return discarded;
}

static void reset_terminal_uplink_for_turn(void)
{
    if (s_uplink_queue_lock != NULL)
        xSemaphoreTake(s_uplink_queue_lock, portMAX_DELAY);
    s_uplink_terminal_active = false;
    s_uplink_terminal_close_pending = false;
    s_uplink_terminal_listen_stop_started = false;
    s_terminal_tail_count = 0U;
    if (s_uplink_queue_lock != NULL)
        xSemaphoreGive(s_uplink_queue_lock);
}

static void begin_terminal_uplink_transition(void)
{
    if (s_uplink_queue_lock != NULL)
        xSemaphoreTake(s_uplink_queue_lock, portMAX_DELAY);
    s_uplink_terminal_active = true;
    s_uplink_terminal_close_pending = false;
    s_uplink_terminal_listen_stop_started = false;
    ++s_uplink_terminal_generation;
    if (s_uplink_terminal_generation == 0U)
        ++s_uplink_terminal_generation;
    s_uplink_enabled = false;
    if (s_uplink_queue_lock != NULL)
        xSemaphoreGive(s_uplink_queue_lock);
    ESP_LOGI(TAG, "terminal uplink begin: turn=%u terminal_gen=%u",
             (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation);
}

static bool begin_main_uplink_send(void)
{
    bool admitted = false;
    if (s_uplink_queue_lock != NULL)
        xSemaphoreTake(s_uplink_queue_lock, portMAX_DELAY);
    if (!s_uplink_terminal_active && (s_uplink_enabled || s_uplink_primer_active))
    {
        s_uplink_bin_in_flight = true;
        admitted = true;
    }
    if (s_uplink_queue_lock != NULL)
        xSemaphoreGive(s_uplink_queue_lock);
    return admitted;
}

static bool finish_main_uplink_send(void)
{
    if (s_uplink_queue_lock != NULL)
        xSemaphoreTake(s_uplink_queue_lock, portMAX_DELAY);
    s_uplink_bin_in_flight = false;
    const bool terminal_deadline = s_uplink_terminal_close_pending;
    s_uplink_terminal_close_pending = false;
    if (s_uplink_queue_lock != NULL)
        xSemaphoreGive(s_uplink_queue_lock);
    return terminal_deadline;
}

static bool wait_for_main_uplink_write(void)
{
    const int64_t wait_start_us = esp_timer_get_time();
    bool tail_deadline_logged = false;
    for (;;)
    {
        if (s_uplink_queue_lock != NULL)
            xSemaphoreTake(s_uplink_queue_lock, portMAX_DELAY);
        const bool in_flight = s_uplink_bin_in_flight;
        const int64_t now_us = esp_timer_get_time();
        if (in_flight && !tail_deadline_logged && now_us >= s_uplink_tail_deadline_us)
        {
            s_uplink_terminal_close_pending = true;
            tail_deadline_logged = true;
            ESP_LOGW(TAG, "terminal tail cutoff passed while bin-main in flight: turn=%u terminal_gen=%u wait_us=%lld in_flight=1 send_result=%d connected=%d",
                     (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation,
                     (long long)(now_us - wait_start_us), s_last_uplink_send_result,
                     s_last_uplink_send_connected ? 1 : 0);
        }
        if (s_uplink_queue_lock != NULL)
            xSemaphoreGive(s_uplink_queue_lock);
        if (!in_flight)
        {
            s_uplink_main_wait_us = now_us - wait_start_us;
            ESP_LOGI(TAG, "in-flight bin-main completed: turn=%u terminal_gen=%u wait_us=%lld in_flight=0 send_result=%d connected=%d after_tail_cutoff=%d",
                     (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation,
                     (long long)(now_us - wait_start_us), s_last_uplink_send_result,
                     s_last_uplink_send_connected ? 1 : 0, tail_deadline_logged ? 1 : 0);
            return true;
        }
        if (now_us - wait_start_us >= XIAOZHI_UPLINK_MAIN_WRITE_WAIT_TIMEOUT_US)
        {
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
    if (s_uplink_queue == NULL)
        return;
    size_t items_waiting = 0U;
    size_t free_size = 0U;
    if (s_uplink_queue_lock != NULL)
        xSemaphoreTake(s_uplink_queue_lock, portMAX_DELAY);
    vRingbufferGetInfo(s_uplink_queue, NULL, NULL, NULL, NULL, &items_waiting);
    free_size = xRingbufferGetCurFreeSize(s_uplink_queue);
    if (s_uplink_queue_lock != NULL)
        xSemaphoreGive(s_uplink_queue_lock);
    ESP_LOGI(TAG, "Opus uplink queue depth: phase=%s items=%u free_bytes=%u terminal_gen=%u",
             phase, (unsigned)items_waiting, (unsigned)free_size,
             (unsigned)s_uplink_terminal_generation);
}

static bool capture_terminal_uplink_tails(void)
{
    uint32_t discarded = 0U;
    s_terminal_tail_count = 0U;
    if (s_uplink_queue == NULL)
        return false;
    log_uplink_queue_depth("terminal-capture-before");
    if (s_uplink_queue_lock != NULL)
        xSemaphoreTake(s_uplink_queue_lock, portMAX_DELAY);
    for (;;)
    {
        size_t item_size = 0U;
        uint8_t *item = xRingbufferReceive(s_uplink_queue, &item_size, 0);
        if (item == NULL)
            break;
        if (item_size == 0U || item_size > XIAOZHI_MAX_OPUS_FRAME_BYTES)
        {
            ++discarded;
        }
        else if (s_terminal_tail_count < XIAOZHI_UPLINK_TAIL_FLUSH_FRAMES)
        {
            const uint8_t tail_index = s_terminal_tail_count++;
            memcpy(s_terminal_tail_frames[tail_index], item, item_size);
            s_terminal_tail_lengths[tail_index] = item_size;
        }
        else
        {
            memcpy(s_terminal_tail_frames[0], s_terminal_tail_frames[1],
                   s_terminal_tail_lengths[1]);
            s_terminal_tail_lengths[0] = s_terminal_tail_lengths[1];
            memcpy(s_terminal_tail_frames[1], item, item_size);
            s_terminal_tail_lengths[1] = item_size;
            ++discarded;
        }
        vRingbufferReturnItem(s_uplink_queue, item);
    }
    if (s_uplink_queue_lock != NULL)
        xSemaphoreGive(s_uplink_queue_lock);
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
    if (s_next_uplink_deadline_us == 0)
    {
        s_next_uplink_deadline_us = now_us + XIAOZHI_OPUS_FRAME_INTERVAL_US;
        return;
    }
    s_next_uplink_deadline_us += XIAOZHI_OPUS_FRAME_INTERVAL_US;
    if (s_next_uplink_deadline_us <= now_us)
    {
        s_next_uplink_deadline_us = now_us + XIAOZHI_OPUS_FRAME_INTERVAL_US;
    }
}

static bool send_ws_text_raw(const char *text)
{
    if (text == NULL || s_ws_tx_lock == NULL)
        return false;
    /* 用带超时的锁，而不是无限等待：上行发送任务可能正持着这把锁卡在
     * send_bin 的写阻塞里（连接已失效）。若主循环在此处无限抢锁，就会被
     * 一起拖死，永远无法消费后续的 WSS 断连事件去播放错误音收场。 */
    if (xSemaphoreTake(s_ws_tx_lock, pdMS_TO_TICKS(300)) != pdTRUE)
    {
        ESP_LOGW(TAG, "WSS text send aborted: tx lock busy");
        return false;
    }
    const bool connected = s_client != NULL && esp_websocket_client_is_connected(s_client);
    const bool healthy = s_wss_health != XIAOZHI_WSS_HEALTH_STALE &&
                         s_wss_health != XIAOZHI_WSS_HEALTH_FAILED;
    const int result = connected && s_client_started && healthy
                           ? esp_websocket_client_send_text(s_client, text, strlen(text), pdMS_TO_TICKS(500))
                           : 0;
    ESP_LOGI(TAG, "WSS text send: result=%d connected=%d healthy=%d session=%s turn=%u payload=%s", result,
             connected ? 1 : 0, healthy ? 1 : 0,
             s_session_id[0] != '\0' ? s_session_id : "-", (unsigned)s_turn_id,
             text != NULL ? text : "-");
    xSemaphoreGive(s_ws_tx_lock);
    if (result <= 0)
    {
        ESP_LOGW(TAG, "cloud text send result=%d", result);
        return false;
    }
    return true;
}

static bool send_text(const char *text)
{
    if (!s_hello_ready)
        return false;
    return send_ws_text_raw(text);
}

static bool request_cloud_exit_prompt(void)
{
    if (!s_hello_ready || s_session_id[0] == '\0' || s_client == NULL ||
        !esp_websocket_client_is_connected(s_client))
    {
        ESP_LOGW(TAG, "exit prompt unavailable: WSS session is not ready");
        return false;
    }
    const uint32_t phrase_count = (uint32_t)(sizeof(s_exit_phrases) / sizeof(s_exit_phrases[0]));
    const uint32_t index = (uint32_t)(esp_random() % phrase_count);
    char request[512];
    (void)snprintf(request, sizeof(request),
                   "{\"type\":\"listen\",\"state\":\"detect\",\"text\":\"[device_call]%s\"}",
                   s_exit_phrases[index]);
    ++s_turn_id;
    if (s_turn_id == 0U)
        ++s_turn_id;
    s_exit_prompt_turn_id = s_turn_id;
    s_exit_prompt_pending = true;
    s_timeout_prompt_pending = false;
    s_timeout_prompt_turn_id = 0U;
    s_fallback_prompt_pending = false;
    s_fallback_prompt_turn_id = 0U;
    s_state = XIAOZHI_VOICE_EXITING;
    const int64_t now_us = esp_timer_get_time();
    s_response_deadline_us = 0;
    /* The original speaking turn is superseded by the generated exit prompt;
     * a late cloud reply for it must not collide with the exit TTS slot. */
    s_waiting_turn_id = 0U;
    s_exit_tts_start_deadline_us = now_us + XIAOZHI_EXIT_TTS_START_TIMEOUT_US;
    s_exit_tts_first_packet_deadline_us = 0;
    s_exit_tts_playback_deadline_us = 0;
    s_wait_for_turn_silence = false;
    xiaozhi_audio_pipeline_reset_after_silence();
    (void)discard_uplink_queue();
    if (!send_text(request))
    {
        s_exit_prompt_pending = false;
        s_exit_prompt_turn_id = 0U;
        s_exit_tts_start_deadline_us = 0;
        ESP_LOGW(TAG, "exit prompt request failed: stage=request_send; ending voice session");
        return false;
    }
    ESP_LOGI(TAG, "exit prompt selected: index=%u text=%s", (unsigned)index, s_exit_phrases[index]);
    ESP_LOGI(TAG, "exit prompt request sent: turn=%u", (unsigned)s_exit_prompt_turn_id);
    s_exit_prompt_retried = false;
    publish_ui_state(VOICE_UI_THINKING);
    return true;
}

static bool request_cloud_timeout_prompt(void)
{
    if (!s_hello_ready || s_session_id[0] == '\0' || s_client == NULL ||
        !esp_websocket_client_is_connected(s_client))
    {
        ESP_LOGW(TAG, "timeout prompt unavailable: WSS session is not ready");
        return false;
    }
    const uint32_t phrase_count = (uint32_t)(sizeof(s_timeout_phrases) / sizeof(s_timeout_phrases[0]));
    const uint32_t index = (uint32_t)(esp_random() % phrase_count);
    char request[512];
    (void)snprintf(request, sizeof(request),
                   "{\"type\":\"listen\",\"state\":\"detect\",\"text\":\"[device_call]%s\"}",
                   s_timeout_phrases[index]);
    ++s_turn_id;
    if (s_turn_id == 0U)
        ++s_turn_id;
    s_timeout_prompt_turn_id = s_turn_id;
    s_timeout_prompt_pending = true;
    s_state = XIAOZHI_VOICE_EXITING;
    const int64_t now_us = esp_timer_get_time();
    s_response_deadline_us = 0;
    /* The timed-out turn is superseded by the generated timeout prompt; a
     * late cloud reply for it must not collide with the prompt TTS slot. */
    s_waiting_turn_id = 0U;
    s_exit_prompt_pending = false;
    s_exit_prompt_turn_id = 0U;
    s_fallback_prompt_pending = false;
    s_fallback_prompt_turn_id = 0U;
    s_exit_tts_start_deadline_us = now_us + XIAOZHI_EXIT_TTS_START_TIMEOUT_US;
    s_exit_tts_first_packet_deadline_us = 0;
    s_exit_tts_playback_deadline_us = 0;
    s_wait_for_turn_silence = false;
    xiaozhi_audio_pipeline_reset_after_silence();
    (void)discard_uplink_queue();
    if (!send_text(request))
    {
        s_timeout_prompt_pending = false;
        s_timeout_prompt_turn_id = 0U;
        s_exit_tts_start_deadline_us = 0;
        ESP_LOGW(TAG, "timeout prompt request failed: stage=request_send; falling back to timeout UI");
        return false;
    }
    ESP_LOGI(TAG, "timeout prompt selected: index=%u text=%s", (unsigned)index, s_timeout_phrases[index]);
    ESP_LOGI(TAG, "timeout prompt request sent: turn=%u", (unsigned)s_timeout_prompt_turn_id);
    s_timeout_prompt_retried = false;
    publish_ui_state(VOICE_UI_THINKING);
    return true;
}

static bool request_cloud_fallback_prompt(void)
{
    if (!s_hello_ready || s_session_id[0] == '\0' || s_client == NULL ||
        !esp_websocket_client_is_connected(s_client))
    {
        ESP_LOGW(TAG, "fallback prompt unavailable: WSS session is not ready");
        return false;
    }
    const uint32_t phrase_count = (uint32_t)(sizeof(s_fallback_phrases) / sizeof(s_fallback_phrases[0]));
    const uint32_t index = (uint32_t)(esp_random() % phrase_count);
    char request[512];
    (void)snprintf(request, sizeof(request),
                   "{\"type\":\"listen\",\"state\":\"detect\",\"text\":\"[device_call]%s\"}",
                   s_fallback_phrases[index]);
    ++s_turn_id;
    if (s_turn_id == 0U)
        ++s_turn_id;
    s_fallback_prompt_turn_id = s_turn_id;
    s_fallback_prompt_pending = true;
    s_state = XIAOZHI_VOICE_EXITING;
    const int64_t now_us = esp_timer_get_time();
    s_response_deadline_us = 0;
    /* The failed turn is superseded by the generated fallback prompt; a late
     * cloud reply for it must not collide with the prompt TTS slot. */
    s_waiting_turn_id = 0U;
    s_exit_prompt_pending = false;
    s_exit_prompt_turn_id = 0U;
    s_timeout_prompt_pending = false;
    s_timeout_prompt_turn_id = 0U;
    s_exit_tts_start_deadline_us = now_us + XIAOZHI_EXIT_TTS_START_TIMEOUT_US;
    s_exit_tts_first_packet_deadline_us = 0;
    s_exit_tts_playback_deadline_us = 0;
    s_wait_for_turn_silence = false;
    xiaozhi_audio_pipeline_reset_after_silence();
    (void)discard_uplink_queue();
    if (!send_text(request))
    {
        s_fallback_prompt_pending = false;
        s_fallback_prompt_turn_id = 0U;
        s_exit_tts_start_deadline_us = 0;
        ESP_LOGW(TAG, "fallback prompt request failed: stage=request_send; ending voice session");
        return false;
    }
    ESP_LOGI(TAG, "fallback prompt selected: index=%u text=%s", (unsigned)index, s_fallback_phrases[index]);
    ESP_LOGI(TAG, "fallback prompt request sent: turn=%u", (unsigned)s_fallback_prompt_turn_id);
    s_fallback_prompt_retried = false;
    publish_ui_state(VOICE_UI_THINKING);
    return true;
}

/* play_error_tone_then_close below may close the session directly when the
 * shared playout refuses the error tone; close_channel is defined later. */
static void close_channel(bool refresh_credentials);

/* Plays the 2 s local error tone (embedded PCM asset) through the shared
 * playout.  Returns false so the caller can close the session directly when
 * the shared playout is unavailable. */
static bool begin_error_tone(void)
{
    extern const uint8_t error_tone_pcm_start[] asm("_binary_error_tone_16k_mono_s16le_pcm_start");
    extern const uint8_t error_tone_pcm_end[] asm("_binary_error_tone_16k_mono_s16le_pcm_end");
    const size_t pcm_bytes = (size_t)(error_tone_pcm_end - error_tone_pcm_start);
    ++s_error_tone_epoch;
    if (s_error_tone_epoch == 0U)
        ++s_error_tone_epoch;
    const uint32_t epoch = s_error_tone_epoch;
    if (audio_playback_service_begin(AUDIO_PLAYOUT_SOURCE_ERROR_TONE, epoch, 8U) != ESP_OK)
    {
        ESP_LOGW(TAG, "error tone begin failed: epoch=%u", (unsigned)epoch);
        return false;
    }
    size_t offset = 0U;
    while (offset < pcm_bytes)
    {
        const size_t chunk = (pcm_bytes - offset) > 2048U ? 2048U : (pcm_bytes - offset);
        if (audio_playback_service_enqueue_pcm(
                AUDIO_PLAYOUT_SOURCE_ERROR_TONE, epoch,
                (const uint8_t *)error_tone_pcm_start + offset, chunk) != ESP_OK)
        {
            ESP_LOGW(TAG, "error tone enqueue failed: epoch=%u offset=%u",
                     (unsigned)epoch, (unsigned)offset);
            audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_ERROR_TONE, epoch);
            return false;
        }
        offset += chunk;
    }
    (void)audio_playback_service_finish(AUDIO_PLAYOUT_SOURCE_ERROR_TONE, epoch);
    ESP_LOGI(TAG, "local error tone queued: epoch=%u bytes=%u", (unsigned)epoch,
             (unsigned)pcm_bytes);
    return true;
}

/* A generated prompt could not be requested (WSS dead) or its cloud TTS
 * timed out after one retry.  Every turn still owes the user an audible
 * answer, so play the local error tone and close the session (returning to
 * the main page) only after it finishes, instead of bouncing back silently. */
static void play_error_tone_then_close(bool show_timeout_ui)
{
    xiaozhi_audio_pipeline_abort_playback();
    /* Drop every prompt slot and deadline so a stale bookkeeping entry can
     * never close or collide with the local error tone below. */
    s_exit_prompt_pending = false;
    s_exit_prompt_turn_id = 0U;
    s_playing_exit_prompt = false;
    s_timeout_prompt_pending = false;
    s_timeout_prompt_turn_id = 0U;
    s_playing_timeout_prompt = false;
    s_fallback_prompt_pending = false;
    s_fallback_prompt_turn_id = 0U;
    s_playing_fallback_prompt = false;
    s_exit_tts_start_deadline_us = 0;
    s_exit_tts_first_packet_deadline_us = 0;
    s_exit_tts_playback_deadline_us = 0;
    s_discard_tts_stream = true;
    s_tts_playback_epoch = 0U;
    s_wait_for_turn_silence = false;
    s_state = XIAOZHI_VOICE_ERROR_TONE;
    s_error_tone_deadline_us = esp_timer_get_time() + XIAOZHI_ERROR_TONE_DEADLINE_US;
    s_timeout_ui_active = true;
    ESP_LOGW(TAG, "prompt unavailable; playing local error tone then closing the session");
    publish_ui_state(show_timeout_ui ? VOICE_UI_TIMEOUT : VOICE_UI_ERROR);
    if (!begin_error_tone())
    {
        ESP_LOGW(TAG, "local error tone unavailable; closing the session directly");
        close_channel(false);
    }
}

static void handle_capture_low_level_if_ready(void)
{
    if (s_capture_low_level_turn_id == 0U ||
        s_state != XIAOZHI_VOICE_WAIT_RESPONSE ||
        s_waiting_turn_id != s_capture_low_level_turn_id)
    {
        return;
    }
    const uint32_t turn_id = s_capture_low_level_turn_id;
    const uint32_t rms = s_capture_low_level_rms;
    const uint32_t peak = s_capture_low_level_peak;
    s_capture_low_level_turn_id = 0U;
    s_capture_low_level_rms = 0U;
    s_capture_low_level_peak = 0U;
    s_response_deadline_us = 0;
    ESP_LOGW(TAG, "capture low level; bypassing cloud response wait: turn=%u rms=%u peak=%u threshold=%u",
             (unsigned)turn_id, (unsigned)rms, (unsigned)peak,
             (unsigned)XIAOZHI_CAPTURE_LOW_LEVEL_RMS);
    if (!request_cloud_timeout_prompt())
        play_error_tone_then_close(true);
}

static void log_uplink_traffic(size_t bytes)
{
    ++s_uplink_frames;
    s_uplink_bytes += bytes;
    const int64_t now_us = esp_timer_get_time();
    if (now_us - s_uplink_log_us < XIAOZHI_TRAFFIC_LOG_INTERVAL_US)
        return;
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
    if (now_us - s_tts_log_us < XIAOZHI_TRAFFIC_LOG_INTERVAL_US)
        return;
    ESP_LOGI(TAG, "cloud TTS: frames=%u bytes=%u", (unsigned)s_tts_frames,
             (unsigned)s_tts_bytes);
    s_tts_frames = 0;
    s_tts_bytes = 0U;
    s_tts_log_us = now_us;
}

static void close_channel(bool refresh_credentials)
{
    if (s_client_cleanup_in_progress)
    {
        ESP_LOGW(TAG, "WSS cleanup already owned; duplicate close ignored");
        return;
    }
    s_client_cleanup_in_progress = true;
    const bool had_active_session = s_session_active;
    if (s_pending_countdown_start)
    {
        const timer_service_cmd_result_t result = timer_service_start_pending_countdowns();
        ESP_LOGI(TAG, "deferred countdown finalized on voice chain exit: result=%d",
                 (int)result);
        s_pending_countdown_start = false;
    }
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
    /* A closing session must also silence the local error tone (e.g. the
     * watchdog or a timer ring closed the turn before it finished). */
    if (s_error_tone_epoch != 0U)
    {
        audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_ERROR_TONE, s_error_tone_epoch);
    }
    s_error_tone_deadline_us = 0;
    s_pending_radio_play = false;
    s_hello_ready = false;
    s_pending_wake = false;
    s_wait_for_turn_silence = false;
    s_post_playback_silence_deadline_us = 0;
    s_capture_low_level_turn_id = 0U;
    s_capture_low_level_rms = 0U;
    s_capture_low_level_peak = 0U;
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
    s_exit_prompt_pending = false;
    s_playing_exit_prompt = false;
    s_exit_prompt_turn_id = 0U;
    s_timeout_prompt_pending = false;
    s_playing_timeout_prompt = false;
    s_timeout_prompt_turn_id = 0U;
    s_fallback_prompt_pending = false;
    s_playing_fallback_prompt = false;
    s_fallback_prompt_turn_id = 0U;
    s_exit_prompt_retried = false;
    s_timeout_prompt_retried = false;
    s_fallback_prompt_retried = false;
    s_exit_tts_start_deadline_us = 0;
    s_exit_tts_first_packet_deadline_us = 0;
    s_exit_tts_playback_deadline_us = 0;
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
    s_tts_stall_deadline_us = 0;
    s_tts_first_packet_seen = false;
    reset_uplink_pacing();
    s_state = XIAOZHI_VOICE_WAIT_WAKE;
    s_session_active = false;
    s_wss_health = XIAOZHI_WSS_HEALTH_CLOSED;
    if (s_timeout_ui_active)
    {
        s_timeout_ui_active = false;
        publish_ui_state(VOICE_UI_IDLE);
    }
    else if (had_active_session)
    {
        publish_ui_state(VOICE_UI_ERROR);
        schedule_ui_idle_after(XIAOZHI_UI_ERROR_HOLD_US);
    }
    else
    {
        publish_ui_state(VOICE_UI_IDLE);
    }
    if (s_client != NULL)
    {
        /* 用带超时的锁，而不是无限等待：上行发送任务可能正持着这把锁卡在
         * send_bin 的写阻塞里（socket 已死）。若主线程在此处无限抢锁，就会被
         * 一起拖死，永远无法走完本地错误音的收场。 */
        const bool lock_ok = (s_ws_tx_lock == NULL) ||
                             (xSemaphoreTake(s_ws_tx_lock, pdMS_TO_TICKS(300)) == pdTRUE);
        if (!lock_ok)
        {
            ESP_LOGW(TAG, "WSS close skipped: tx lock busy (uplink stuck in send_bin)");
        }
        else
        {
            /* Clear the lifecycle flag before stop().  The websocket component
             * may synchronously emit DISCONNECTED/ERROR from inside stop(). */
            const bool was_started = s_client_started;
            s_client_started = false;
            if (was_started)
                (void)esp_websocket_client_stop(s_client);
            (void)esp_websocket_client_destroy(s_client);
            s_client = NULL;
            s_client_started = false;
            if (s_ws_tx_lock != NULL)
                xSemaphoreGive(s_ws_tx_lock);
        }
    }
    if (refresh_credentials)
        xiaozhi_activation_service_refresh_credentials();
    if (s_initialized && xiaozhi_activation_service_is_activated())
    {
        s_prewarm_retry_us = esp_timer_get_time() + XIAOZHI_PREWARM_RETRY_US;
        s_prewarm_retry_delay_us = 0; /* fresh schedule: reset the backoff ramp */
        ESP_LOGI(TAG, "WSS idle prewarm scheduled: delay_ms=%u", (unsigned)(XIAOZHI_PREWARM_RETRY_US / 1000LL));
    }
    s_client_cleanup_in_progress = false;
}

static bool send_uplink_packet(const uint8_t *opus, size_t opus_len, const char *operation)
{
    if (s_ws_tx_lock == NULL)
        return false;
    s_uplink_tx_operation = operation;
    xSemaphoreTake(s_ws_tx_lock, portMAX_DELAY);
    const bool connected_before = s_client != NULL && s_client_started &&
                                  esp_websocket_client_is_connected(s_client);
    const int64_t now_us = esp_timer_get_time();
    const bool heartbeat_stale = s_wss_last_pong_us != 0 &&
                                 now_us - s_wss_last_pong_us > 20LL * 1000LL * 1000LL;
    if (heartbeat_stale)
        s_wss_health = XIAOZHI_WSS_HEALTH_STALE;
    s_last_uplink_send_connected = connected_before;
    s_last_uplink_send_result = s_last_uplink_send_connected && s_wss_health != XIAOZHI_WSS_HEALTH_STALE && s_wss_health != XIAOZHI_WSS_HEALTH_FAILED
                                    ? esp_websocket_client_send_bin(
                                          s_client, (const char *)opus, (int)opus_len,
                                          pdMS_TO_TICKS(1000))
                                    : 0;
    const bool connected_after = s_client != NULL && s_client_started &&
                                 esp_websocket_client_is_connected(s_client);
    xSemaphoreGive(s_ws_tx_lock);
    ESP_LOGI(TAG, "Opus uplink op=%s result=%d connected_before=%d connected_after=%d health=%d state=%d turn=%u terminal_gen=%u bytes=%u timeout_ms=1000",
             operation,
             s_last_uplink_send_result, connected_before ? 1 : 0, connected_after ? 1 : 0,
             s_wss_health,
             s_state, (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation,
             (unsigned)opus_len);
    s_uplink_tx_operation = "idle";
    if (s_last_uplink_send_result > 0)
        return true;
    /* A failed write invalidates this session even if the websocket library
     * still reports it connected for a short callback window. */
    s_wss_health = XIAOZHI_WSS_HEALTH_FAILED;
    ESP_LOGE(TAG, "voice turn transport failure: op=%s turn=%u frames=%u connected=%d result=%d",
             operation,
             (unsigned)s_turn_id, (unsigned)s_uplink_frames,
             s_last_uplink_send_connected ? 1 : 0, s_last_uplink_send_result);
    const int64_t pong_age_us = s_wss_last_pong_us == 0 ? -1 : now_us - s_wss_last_pong_us;
    ESP_LOGE(TAG, "uplink failure context: op=%s bytes=%u health=%d connected_before=%d internal=%u largest=%u last_pong_age_us=%lld tls_esp=%d tls_stack=%d tls_flags=%d sock_errno=%d close_code=%d",
             operation, (unsigned)opus_len, s_wss_health,
             connected_before ? 1 : 0,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (long long)pong_age_us,
             (int)s_wss_last_error.esp_tls_last_esp_err,
             s_wss_last_error.esp_tls_stack_err,
             s_wss_last_error.esp_tls_cert_verify_flags,
             s_wss_last_error.esp_transport_sock_errno,
             s_wss_last_close_code);
    log_wss_wifi_state();
    return false;
}

static bool send_listen_start(void)
{
    if (s_session_id[0] == '\0')
    {
        ESP_LOGW(TAG, "listen/start not sent: current WSS session_id is missing");
        return false;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL || cJSON_AddStringToObject(root, "type", "listen") == NULL ||
        cJSON_AddStringToObject(root, "state", "start") == NULL ||
        cJSON_AddStringToObject(root, "mode", "manual") == NULL ||
        cJSON_AddStringToObject(root, "session_id", s_session_id) == NULL)
    {
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
    if (s_session_id[0] == '\0')
    {
        ESP_LOGW(TAG, "listen/stop not sent: current WSS session_id is missing");
        return false;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL || cJSON_AddStringToObject(root, "type", "listen") == NULL ||
        cJSON_AddStringToObject(root, "state", "stop") == NULL ||
        cJSON_AddStringToObject(root, "session_id", s_session_id) == NULL)
    {
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
    if (!s_listen_start_sent_for_turn)
    {
        ++s_turn_id;
        if (s_turn_id == 0U)
            ++s_turn_id;
        s_waiting_turn_id = 0U;
        if (!send_listen_start())
            return false;
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
    if (xiaozhi_audio_pipeline_queue_silence_opus_frames(XIAOZHI_WAKE_PRIMER_FRAMES) != ESP_OK)
    {
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
    if (s_state != XIAOZHI_VOICE_WAIT_SPEECH)
        return;
    s_capture_low_level_turn_id = 0U;
    s_capture_low_level_rms = 0U;
    s_capture_low_level_peak = 0U;
    if (!s_listen_start_sent_for_turn)
    {
        ++s_turn_id;
        if (s_turn_id == 0U)
            ++s_turn_id;
        s_waiting_turn_id = 0U;
        s_playing_turn_id = 0U;
        if (!send_listen_start())
        {
            ESP_LOGW(TAG, "listen/start send failed; requesting generated fallback prompt");
            xiaozhi_audio_pipeline_reset_after_transport_failure();
            if (!request_cloud_fallback_prompt())
                play_error_tone_then_close(false);
            return;
        }
        s_listen_start_sent_for_turn = true;
        reset_uplink_pacing();
        (void)discard_uplink_queue();
        s_uplink_failed = false;
        reset_terminal_uplink_for_turn();
    }
    if (!enable_capture)
    {
        if (!start_wake_server_ignore_primer())
        {
            ESP_LOGW(TAG, "wake server ignore primer failed; requesting generated fallback prompt");
            if (!request_cloud_fallback_prompt())
                play_error_tone_then_close(false);
        }
        return;
    }
    s_uplink_primer_active = false;
    s_uplink_primer_frames_remaining = 0U;
    s_uplink_enabled = true;
    notify_uplink_task();
    if (xiaozhi_audio_pipeline_set_streaming(true) != ESP_OK)
    {
        ESP_LOGW(TAG, "unable to start Opus uplink; requesting generated fallback prompt");
        if (!request_cloud_fallback_prompt())
            play_error_tone_then_close(false);
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
    if (hour < 11)
        return "早上好";
    if (hour < 14)
        return "中午好";
    if (hour < 18)
        return "下午好";
    return "晚上好";
}

static bool build_boot_greeting_text(char *greeting, size_t greeting_size)
{
    lumidock_snapshot_t snapshot = {0};
    mid_snapshot_get(&snapshot);

    if (!snapshot.time_weather.valid || snapshot.time_weather.weather_text[0] == '\0')
    {
        const int written = snprintf(greeting, greeting_size,
                                     "[device_call]你好呀，很高兴见到你，今天有什么想和我聊的吗？");
        return written >= 0 && (size_t)written < greeting_size;
    }

    const struct tm *local_time = &snapshot.time_weather.local_time;
    const int temperature_c = snapshot.time_weather.temperature_c >= 0.0f ? (int)(snapshot.time_weather.temperature_c + 0.5f) : (int)(snapshot.time_weather.temperature_c - 0.5f);
    const uint32_t template_index = (uint32_t)snapshot.time_weather.local_time.tm_yday % 3U;
    const char *weather = snapshot.time_weather.weather_text;
    int written;
    if (template_index == 0U)
    {
        written = snprintf(greeting, greeting_size,
                           "[device_call]%s，现在是%d点%02d分，今天%s，气温%d度。今天有什么安排吗？",
                           boot_greeting_salutation(local_time->tm_hour), local_time->tm_hour,
                           local_time->tm_min, weather, temperature_c);
    }
    else if (template_index == 1U)
    {
        written = snprintf(greeting, greeting_size,
                           "[device_call]今天是星期%d，当前%d点%02d分，天气%s，温度%d度。出门记得留意天气哦。",
                           local_time->tm_wday, local_time->tm_hour, local_time->tm_min,
                           weather, temperature_c);
    }
    else
    {
        written = snprintf(greeting, greeting_size,
                           "[device_call]你好呀，现在%d点%02d分，今天%s，%d度。想让我帮你查点什么吗？",
                           local_time->tm_hour, local_time->tm_min, weather, temperature_c);
    }
    return written >= 0 && (size_t)written < greeting_size;
}

static bool send_boot_greeting_detect(void)
{
    char greeting[XIAOZHI_BOOT_GREETING_TEXT_MAX_BYTES] = {0};
    if (!build_boot_greeting_text(greeting, sizeof(greeting)))
    {
        ESP_LOGW(TAG, "boot greeting text generation failed");
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL || cJSON_AddStringToObject(root, "type", "listen") == NULL ||
        cJSON_AddStringToObject(root, "state", "detect") == NULL ||
        cJSON_AddStringToObject(root, "text", greeting) == NULL)
    {
        cJSON_Delete(root);
        return false;
    }
    char *request = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (request == NULL)
        return false;

    ESP_LOGI(TAG, "boot greeting text prepared: %s", greeting);
    const bool sent = send_text(request);
    cJSON_free(request);
    return sent;
}

static void begin_boot_greeting(void)
{
    s_boot_greeting_request_pending = false;
    if (s_boot_greeting_attempted)
        return;
    s_boot_greeting_attempted = true;
    if (!s_hello_ready || s_client == NULL || !esp_websocket_client_is_connected(s_client))
    {
        finish_boot_greeting_failure("WSS not ready");
        return;
    }
    if (!send_boot_greeting_detect())
    {
        finish_boot_greeting_failure("listen/detect send failed");
        return;
    }
    ++s_turn_id;
    if (s_turn_id == 0U)
        ++s_turn_id;
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
    for (uint8_t index = 0U; index < s_terminal_tail_count; ++index)
    {
        if (esp_timer_get_time() >= s_uplink_tail_deadline_us)
        {
            ESP_LOGI(TAG, "terminal uplink deadline reached: turn=%u terminal_gen=%u sent_tails=%u retained_tails=%u",
                     (unsigned)s_turn_id, (unsigned)terminal_generation, (unsigned)index,
                     (unsigned)s_terminal_tail_count);
            break;
        }
        if (!send_uplink_packet(s_terminal_tail_frames[index], s_terminal_tail_lengths[index], "bin-tail"))
        {
            ESP_LOGW(TAG, "terminal uplink tail failed: turn=%u terminal_gen=%u tail=%u; requesting generated fallback prompt",
                     (unsigned)s_turn_id, (unsigned)terminal_generation, (unsigned)index);
            if (!request_cloud_fallback_prompt())
                play_error_tone_then_close(false);
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
    if (!stopped)
    {
        ESP_LOGW(TAG, "terminal listen/stop failed: turn=%u terminal_gen=%u; requesting generated fallback prompt",
                 (unsigned)s_turn_id, (unsigned)terminal_generation);
        if (!request_cloud_fallback_prompt())
            play_error_tone_then_close(false);
        return false;
    }
    xiaozhi_audio_pipeline_reset_wake_word();
    reset_uplink_pacing();
    s_waiting_turn_id = s_turn_id;
    s_playing_turn_id = 0U;
    s_listen_start_sent_for_turn = false;
    s_response_deadline_us = esp_timer_get_time() + XIAOZHI_WAIT_RESPONSE_TIMEOUT_US;
    s_state = XIAOZHI_VOICE_WAIT_RESPONSE;
    handle_capture_low_level_if_ready();
    if (s_state != XIAOZHI_VOICE_WAIT_RESPONSE)
        return true;
    ESP_LOGI(TAG, "voice VAD: speech ended; waiting for cloud response turn=%u terminal_gen=%u",
             (unsigned)s_waiting_turn_id, (unsigned)terminal_generation);
    publish_ui_state(VOICE_UI_THINKING);
    return true;
}

static void finish_listen_with_tail_flush(void)
{
    if (s_state != XIAOZHI_VOICE_LISTENING)
        return;
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
    if (!wait_for_main_uplink_write())
    {
        ESP_LOGW(TAG, "terminal main write wait timed out: turn=%u terminal_gen=%u in_flight=%d wait_us=%lld send_result=%d connected=%d",
                 (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation,
                 s_uplink_bin_in_flight ? 1 : 0, (long long)s_uplink_main_wait_us,
                 s_last_uplink_send_result,
                 s_last_uplink_send_connected ? 1 : 0);
        if (!request_cloud_fallback_prompt())
            play_error_tone_then_close(false); /* 先走云 fallback；WSS 不可用则回退本地故障提示音收场 */
        return;
    }
    if (s_uplink_failed || s_last_uplink_send_result <= 0 || !capture_terminal_uplink_tails())
    {
        ESP_LOGW(TAG, "terminal main write failed: turn=%u terminal_gen=%u in_flight=%d send_result=%d connected=%d; requesting generated fallback prompt",
                 (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation,
                 s_uplink_bin_in_flight ? 1 : 0, s_last_uplink_send_result,
                 s_last_uplink_send_connected ? 1 : 0);
        if (!request_cloud_fallback_prompt())
            play_error_tone_then_close(false);
        return;
    }
    (void)send_terminal_uplink_sequence();
}

static void send_detect(bool start_current_speech)
{
    if (!send_text("{\"type\":\"listen\",\"state\":\"detect\",\"text\":\"你好,小智\"}"))
        return;
    ++s_turn_id;
    if (s_turn_id == 0U)
        ++s_turn_id;
    s_greeting_turn_id = s_turn_id;
    s_waiting_turn_id = 0U;
    s_playing_turn_id = 0U;
    s_state = XIAOZHI_VOICE_WAIT_SPEECH;
    ESP_LOGI(TAG, "voice wake accepted; waiting for speech greeting_turn=%u",
             (unsigned)s_greeting_turn_id);
    publish_ui_state(VOICE_UI_LISTENING);
    if (start_current_speech)
        start_listen(true);
}

/* A follow-up already belongs to the established cloud session.  Sending a
 * second detect frame would make the fixed wake phrase look like user input. */
static void start_followup_listen(void)
{
    s_state = XIAOZHI_VOICE_WAIT_SPEECH;
    start_listen(true);
}

static void arm_post_playback_silence_gate(void)
{
    s_wait_for_turn_silence = true;
    s_post_playback_silence_deadline_us = esp_timer_get_time() +
                                          XIAOZHI_POST_PLAYBACK_SILENCE_GUARD_US;
    ESP_LOGI(TAG, "post-playback silence gate armed: guard_ms=%u reset_peak=%u bypass_peak=%u",
             (unsigned)(XIAOZHI_POST_PLAYBACK_SILENCE_GUARD_US / 1000LL),
             (unsigned)XIAOZHI_POST_PLAYBACK_SILENCE_RESET_PEAK,
             (unsigned)XIAOZHI_DIRECT_REPLY_BYPASS_PEAK);
}

static bool consume_post_playback_vad_start(uint32_t recent_peak)
{
    const bool boot_direct_reply = s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY;
    const bool followup_reply = s_state == XIAOZHI_VOICE_WAIT_FOLLOWUP;
    if (!s_wait_for_turn_silence || (!boot_direct_reply && !followup_reply))
        return false;

    if (recent_peak < XIAOZHI_POST_PLAYBACK_SILENCE_RESET_PEAK)
    {
        ESP_LOGI(TAG, "post-playback VAD start ignored as echo: peak=%u threshold=%u",
                 (unsigned)recent_peak, (unsigned)XIAOZHI_POST_PLAYBACK_SILENCE_RESET_PEAK);
        return true;
    }
    if (recent_peak < XIAOZHI_DIRECT_REPLY_BYPASS_PEAK)
    {
        s_post_playback_silence_deadline_us = esp_timer_get_time() +
                                              XIAOZHI_POST_PLAYBACK_SILENCE_GUARD_US;
        ESP_LOGI(TAG, "post-playback VAD start retained silence guard: peak=%u deadline_extended_ms=%u",
                 (unsigned)recent_peak,
                 (unsigned)(XIAOZHI_POST_PLAYBACK_SILENCE_GUARD_US / 1000LL));
        return true;
    }

    s_wait_for_turn_silence = false;
    s_post_playback_silence_deadline_us = 0;
    xiaozhi_audio_pipeline_reset_after_silence();
    if (boot_direct_reply)
    {
        s_boot_greeting_reply_deadline_us = 0;
        s_state = XIAOZHI_VOICE_WAIT_SPEECH;
        start_listen(true);
        ESP_LOGI(TAG, "boot greeting high-energy direct reply accepted: peak=%u", (unsigned)recent_peak);
    }
    else
    {
        start_followup_listen();
        s_followup_deadline_us = 0;
        ESP_LOGI(TAG, "follow-up high-energy direct reply accepted: peak=%u", (unsigned)recent_peak);
    }
    return true;
}

static void finish_wake_greeting_after_playback(void)
{
    /* Manual wake follows the same handoff as the boot greeting.  The
     * greeting has already drained, so discard any stale primer bookkeeping,
     * reset the AFE baseline, and wait for a new VAD boundary before opening
     * listen/start.  This keeps speech spoken during the greeting from being
     * mistaken for the user's first command. */
    s_waiting_for_wake_primer = false;
    s_wake_primer_candidate_speech = false;
    s_wake_primer_deadline_us = 0;
    s_state = XIAOZHI_VOICE_WAIT_FOLLOWUP;
    arm_post_playback_silence_gate();
    s_followup_deadline_us = esp_timer_get_time() + XIAOZHI_FOLLOWUP_WINDOW_US;
    ESP_LOGI(TAG, "wake greeting PCM drained; fresh VAD baseline, follow-up window=15s");
}

static void enter_followup_after_playback(void)
{
    const bool exit_prompt_finished = s_playing_exit_prompt;
    const bool timeout_prompt_finished = s_playing_timeout_prompt;
    const bool fallback_prompt_finished = s_playing_fallback_prompt;
    const bool boot_greeting_finished = s_playing_boot_greeting;
    const bool wake_greeting_finished = s_playing_wake_greeting;
    const bool boot_greeting_window_active = boot_greeting_finished ||
                                             s_boot_greeting_direct_window_active;
    s_playing_turn_id = 0U;
    s_playing_boot_greeting = false;
    s_playing_wake_greeting = false;
    s_playing_exit_prompt = false;
    s_playing_timeout_prompt = false;
    s_playing_fallback_prompt = false;
    s_timeout_prompt_turn_id = 0U;
    s_fallback_prompt_turn_id = 0U;
    s_exit_tts_start_deadline_us = 0;
    s_exit_tts_first_packet_deadline_us = 0;
    s_exit_tts_playback_deadline_us = 0;
    s_discard_tts_stream = false;
    s_tts_playback_epoch = 0U;
    if (exit_prompt_finished)
    {
        ESP_LOGI(TAG, "exit prompt PCM drained; closing voice session");
        close_channel(false);
        return;
    }
    if (timeout_prompt_finished)
    {
        ESP_LOGI(TAG, "timeout prompt PCM drained; follow-up window=15s");
    }
    if (fallback_prompt_finished)
    {
        ESP_LOGI(TAG, "fallback prompt PCM drained; follow-up window=15s");
    }
    s_vad_speech = false;
    arm_post_playback_silence_gate();
    if (boot_greeting_finished)
        s_boot_greeting_direct_window_active = true;
    if (boot_greeting_window_active)
    {
        s_boot_greeting_reply_deadline_us = esp_timer_get_time() +
                                            XIAOZHI_BOOT_GREETING_REPLY_WINDOW_US;
        s_state = XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY;
        s_ui_idle_deadline_us = 0;
        if (boot_greeting_finished)
        {
            ESP_LOGI(TAG, "boot greeting PCM drained; direct reply window=15s");
        }
        else
        {
            ESP_LOGI(TAG, "boot greeting response PCM drained; direct reply window re-armed=15s");
        }
    }
    else
    {
        s_state = XIAOZHI_VOICE_WAIT_FOLLOWUP;
        s_followup_deadline_us = esp_timer_get_time() + XIAOZHI_FOLLOWUP_WINDOW_US;
        s_ui_idle_deadline_us = 0;
        ESP_LOGI(TAG, "cloud TTS PCM drained; fresh VAD baseline, follow-up window=15s");
    }
    ESP_LOGI(TAG, "post-playback silence gate armed");
    /* Keep the established WSS/session for the next turn. A follow-up is
     * opened only when the original connection is still healthy; a dead
     * connection ends this turn instead of replaying audio on a new session. */
    const bool wss_ready = s_client != NULL && s_hello_ready &&
                           esp_websocket_client_is_connected(s_client);
    if (!wss_ready)
    {
        ESP_LOGW(TAG, "follow-up WSS health check failed after PCM drain; ending turn");
        xiaozhi_audio_pipeline_reset_after_transport_failure();
        close_channel(false);
        return;
    }
    xiaozhi_audio_pipeline_resume_after_playback();
    xiaozhi_audio_pipeline_arm_followup();
    if (wake_greeting_finished && !boot_greeting_window_active)
    {
        finish_wake_greeting_after_playback();
    }
    publish_ui_state(VOICE_UI_LISTENING);
}

static esp_err_t start_wss(bool foreground_session)
{
    if (s_client != NULL && s_hello_ready && esp_websocket_client_is_connected(s_client))
    {
        if (foreground_session)
        {
            s_session_active = true;
            publish_ui_state(VOICE_UI_CONNECTING);
        }
        return ESP_OK;
    }
    if (s_client != NULL && s_client_started)
    {
        if (esp_websocket_client_is_connected(s_client))
        {
            if (foreground_session)
            {
                s_session_active = true;
                publish_ui_state(VOICE_UI_CONNECTING);
            }
            return ESP_OK;
        }
        ESP_LOGW(TAG, "replacing stale started-but-disconnected WSS client: seq=%u",
                 (unsigned)s_wss_connection_seq);
        if (s_ws_tx_lock != NULL &&
            xSemaphoreTake(s_ws_tx_lock, pdMS_TO_TICKS(300)) != pdTRUE)
        {
            ESP_LOGW(TAG, "WSS stale-client replace skipped: tx lock busy (uplink stuck in send_bin)");
        }
        else
        {
            const bool was_started = s_client_started;
            s_client_started = false;
            if (was_started)
                (void)esp_websocket_client_stop(s_client);
            (void)esp_websocket_client_destroy(s_client);
            s_client = NULL;
            if (s_ws_tx_lock != NULL)
                xSemaphoreGive(s_ws_tx_lock);
            s_hello_ready = false;
            s_session_id[0] = '\0';
        }
    }
    if (s_client != NULL)
        close_channel(false);

    const char *url = xiaozhi_activation_service_get_websocket_url();
    const char *token = xiaozhi_activation_service_get_websocket_token();
    const int authorization_len = snprintf(s_authorization, sizeof(s_authorization), "Bearer %s", token);
    if (url[0] == '\0' || token[0] == '\0' || authorization_len < 0 ||
        (size_t)authorization_len >= sizeof(s_authorization))
    {
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
        .ping_interval_sec = 10,
        .pingpong_timeout_sec = 30,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    ++s_wss_connection_seq;
    if (s_wss_connection_seq == 0U)
        ++s_wss_connection_seq;
    ESP_LOGI(TAG, "WSS diagnostic build: transport-task-boundary-v1");
    ESP_LOGI(TAG, "WSS connection start: seq=%u url=%s", (unsigned)s_wss_connection_seq, url);
    ESP_LOGI(TAG, "WSS dynamic RX/TX buffers enabled; buffers allocate after task creation");
    ESP_LOGI(TAG, "WSS client allocation begin: internal=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    s_client = esp_websocket_client_init(&config);
    if (s_client == NULL)
    {
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
    if (foreground_session)
        publish_ui_state(VOICE_UI_CONNECTING);
    s_hello_deadline_us = esp_timer_get_time() + XIAOZHI_HELLO_TIMEOUT_US;
    ESP_LOGI(TAG, "WSS start memory: stack=%u internal=%u largest=%u",
             4096U,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (esp_websocket_client_start(s_client) != ESP_OK)
    {
        ESP_LOGE(TAG, "WSS task/transport start failed: internal=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        // A transport/TLS startup failure is transient; keep valid activation credentials.
        close_channel(false);
        return ESP_FAIL;
    }
    s_client_started = true;
    s_prewarm_retry_delay_us = 0;
    return ESP_OK;
}

static void prewarm_wss(void)
{
    if (!xiaozhi_activation_service_is_activated() || s_client != NULL)
        return;
    ESP_LOGI(TAG, "prewarming XiaoZhi WSS before wake-word memory fragmentation");
    if (start_wss(false) != ESP_OK)
    {
        /* Prewarm failure is typically a transient DNS/TLS connect problem.
         * Back off exponentially (5s base, 30s cap) instead of hammering a
         * degraded resolver, then schedule the next attempt. */
        if (s_prewarm_retry_delay_us == 0)
            s_prewarm_retry_delay_us = XIAOZHI_PREWARM_RETRY_US;
        else
        {
            s_prewarm_retry_delay_us *= 2;
            if (s_prewarm_retry_delay_us > XIAOZHI_PREWARM_RETRY_MAX_US)
                s_prewarm_retry_delay_us = XIAOZHI_PREWARM_RETRY_MAX_US;
        }
        s_prewarm_retry_us = esp_timer_get_time() + s_prewarm_retry_delay_us;
        ESP_LOGW(TAG, "prewarm failed; retry in %lld ms", (long long)(s_prewarm_retry_delay_us / 1000LL));
    }
}

static void begin_channel(void)
{
    if (!xiaozhi_activation_service_is_activated())
    {
        ESP_LOGW(TAG, "wake ignored: XiaoZhi activation/token unavailable");
        xiaozhi_audio_pipeline_reset_wake_word();
        return;
    }
    s_session_active = true;
    char radio_failure_stage[24];
    int radio_failure_code = 0;
    uint8_t radio_failure_reconnects = 0U;
    if (radio_stream_service_consume_failure(radio_failure_stage, sizeof(radio_failure_stage),
                                             &radio_failure_code, &radio_failure_reconnects))
    {
        ESP_LOGI(TAG, "previous radio failure consumed on wake: stage=%s code=%d reconnects=%u",
                 radio_failure_stage, radio_failure_code, (unsigned)radio_failure_reconnects);
    }
    s_pending_wake = true;
    s_wait_for_turn_silence = true;
    s_followup_deadline_us = 0;
    publish_ui_state(VOICE_UI_CONNECTING);
    if (s_client != NULL && s_hello_ready && esp_websocket_client_is_connected(s_client))
    {
        send_detect(false);
        s_pending_wake = false;
        return;
    }
    if (start_wss(true) != ESP_OK)
    {
        s_pending_wake = false;
        ESP_LOGW(TAG, "foreground WSS start failed; showing voice connection error");
        close_channel(false);
    }
}

static void process_text(const uint8_t *data, size_t data_len)
{
    char message[XIAOZHI_TEXT_MAX_BYTES + 1U];
    if (data_len > XIAOZHI_TEXT_MAX_BYTES)
        return;
    memcpy(message, data, data_len);
    message[data_len] = '\0';
    ESP_LOGI(TAG, "cloud JSON received: %s", message);
    cJSON *root = cJSON_Parse(message);
    cJSON *type = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, "type") : NULL;
    if (!cJSON_IsString(type) || type->valuestring == NULL)
    {
        cJSON_Delete(root);
        return;
    }
    cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const char *state_text = cJSON_IsString(state) && state->valuestring != NULL ? state->valuestring : "-";
    if (strcmp(type->valuestring, "stt") == 0)
    {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (s_state == XIAOZHI_VOICE_WAIT_RESPONSE && cJSON_IsString(text) &&
            text->valuestring != NULL)
        {
            ESP_LOGI(TAG, "local IoT command candidate: text=%s", text->valuestring);
            s_response_deadline_us = esp_timer_get_time() + XIAOZHI_WAIT_RESPONSE_TIMEOUT_US;
            ESP_LOGI(TAG, "cloud STT received; response timeout refreshed turn=%u",
                     (unsigned)s_waiting_turn_id);
            char announce[96];
            announce[0] = '\0';
            const voice_iot_command_result_t result =
                voice_iot_command_service_execute(text->valuestring, announce,
                                                  sizeof(announce));
            if (result != VOICE_IOT_COMMAND_NOT_HANDLED)
            {
                ESP_LOGI(TAG, "local IoT command handled: result=%d text=%s", (int)result,
                         text->valuestring);
                if (result == VOICE_IOT_COMMAND_COUNTDOWN_PENDING)
                {
                    s_pending_countdown_start = true;
                    ESP_LOGI(TAG, "countdown start deferred until cloud TTS completion: turn=%u",
                             (unsigned)s_waiting_turn_id);
                }
                /* The cloud's response is the only confirmation spoken.  A
                 * radio command still starts after that response drains. */
                if (result == VOICE_IOT_COMMAND_RADIO_PLAY)
                {
                    s_pending_radio_play = true;
                }
            }
        }
    }
    else if (strcmp(type->valuestring, "hello") == 0)
    {
        s_hello_ready = true;
        s_hello_deadline_us = 0;
        /* The UI reads a protected snapshot rather than this task's private
         * handshake flag. Publish immediately so startup cannot time out on
         * a stale connected=false snapshot. */
        publish_ui_state(VOICE_UI_CONNECTING);
        const cJSON *session_id = cJSON_GetObjectItemCaseSensitive(root, "session_id");
        if (cJSON_IsString(session_id) && session_id->valuestring != NULL)
        {
            strncpy(s_session_id, session_id->valuestring, sizeof(s_session_id) - 1U);
            s_session_id[sizeof(s_session_id) - 1U] = '\0';
            ESP_LOGI(TAG, "WSS hello session_id saved: %s", s_session_id);
        }
        else
        {
            s_session_id[0] = '\0';
            ESP_LOGW(TAG, "WSS hello did not provide session_id");
        }
        if (s_pending_wake)
        {
            s_state = XIAOZHI_VOICE_WAIT_WAKE;
            /* A wake queued during the prewarm handshake must activate the
             * session here; send_detect() publishes LISTENING using this flag,
             * and without it the UI snapshot stays active=false so the voice
             * page is never entered. */
            s_session_active = true;
            send_detect(false);
            s_pending_wake = false;
        }
        else
        {
            s_state = XIAOZHI_VOICE_WAIT_WAKE;
            ESP_LOGI(TAG, "XiaoZhi WSS prewarm ready; waiting for wake word");
            /* Only a pure prewarm (no pending wake) ends idle; a wake path
             * must keep the LISTENING state published by send_detect(). */
            publish_ui_state(VOICE_UI_IDLE);
        }
    }
    else if (strcmp(type->valuestring, "tts") == 0)
    {
        if (cJSON_IsString(state) && strcmp(state->valuestring, "start") == 0)
        {
            const bool response_turn = s_state == XIAOZHI_VOICE_WAIT_RESPONSE &&
                                       s_waiting_turn_id != 0U;
            const bool greeting_turn = s_state == XIAOZHI_VOICE_WAIT_SPEECH &&
                                       s_greeting_turn_id != 0U;
            const bool boot_greeting_turn = s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_TTS &&
                                            s_boot_greeting_id != 0U;
            const bool exit_turn = s_state == XIAOZHI_VOICE_EXITING &&
                                   s_exit_prompt_pending && s_exit_prompt_turn_id != 0U;
            const bool timeout_turn = s_state == XIAOZHI_VOICE_EXITING &&
                                      s_timeout_prompt_pending && s_timeout_prompt_turn_id != 0U;
            const bool fallback_turn = s_state == XIAOZHI_VOICE_EXITING &&
                                       s_fallback_prompt_pending && s_fallback_prompt_turn_id != 0U;
            if (!response_turn && !greeting_turn && !boot_greeting_turn && !exit_turn &&
                !timeout_turn && !fallback_turn)
            {
                s_discard_tts_stream = true;
                ESP_LOGW(TAG, "duplicate/stale TTS start ignored: state=%d waiting_turn=%u playing_turn=%u",
                         s_state, (unsigned)s_waiting_turn_id, (unsigned)s_playing_turn_id);
                ESP_LOGI(TAG, "cloud JSON processed: type=%s state=%s",
                         type->valuestring, state_text);
                cJSON_Delete(root);
                return;
            }
            s_playing_turn_id = response_turn ? s_waiting_turn_id : (greeting_turn ? s_greeting_turn_id : (boot_greeting_turn ? s_boot_greeting_id : (exit_turn ? s_exit_prompt_turn_id : (timeout_turn ? s_timeout_prompt_turn_id : s_fallback_prompt_turn_id))));
            s_waiting_turn_id = 0U;
            s_greeting_turn_id = 0U;
            s_boot_greeting_id = 0U;
            s_discard_tts_stream = false;
            s_playing_boot_greeting = boot_greeting_turn;
            s_playing_wake_greeting = greeting_turn;
            s_playing_exit_prompt = exit_turn;
            s_playing_timeout_prompt = timeout_turn;
            s_playing_fallback_prompt = fallback_turn;
            s_exit_prompt_pending = false;
            s_timeout_prompt_pending = false;
            s_fallback_prompt_pending = false;
            const bool rejected_generated_prompt = exit_turn || timeout_turn || fallback_turn;
            if (xiaozhi_audio_pipeline_begin_tts_playback(&s_tts_playback_epoch) != ESP_OK)
            {
                s_discard_tts_stream = true;
                s_playing_turn_id = 0U;
                s_playing_boot_greeting = false;
                s_playing_wake_greeting = false;
                s_playing_exit_prompt = false;
                s_playing_timeout_prompt = false;
                s_playing_fallback_prompt = false;
                s_exit_prompt_turn_id = 0U;
                s_timeout_prompt_turn_id = 0U;
                s_fallback_prompt_turn_id = 0U;
                s_exit_tts_start_deadline_us = 0;
                s_exit_tts_first_packet_deadline_us = 0;
                s_exit_tts_playback_deadline_us = 0;
                if (rejected_generated_prompt)
                {
                    /* A generated prompt whose playout is busy must not retry:
                     * the playout would reject the retry as well. */
                    s_state = XIAOZHI_VOICE_WAIT_WAKE;
                    ESP_LOGW(TAG, "cloud TTS rejected: shared PCM playout unavailable");
                    publish_ui_state(VOICE_UI_ERROR);
                    schedule_ui_idle_after(XIAOZHI_UI_ERROR_HOLD_US);
                }
                else
                {
                    ESP_LOGW(TAG, "cloud TTS rejected: shared PCM playout unavailable; requesting generated fallback prompt");
                    if (!request_cloud_fallback_prompt())
                    {
                        s_state = XIAOZHI_VOICE_WAIT_WAKE;
                        publish_ui_state(VOICE_UI_ERROR);
                        schedule_ui_idle_after(XIAOZHI_UI_ERROR_HOLD_US);
                    }
                }
                ESP_LOGI(TAG, "cloud JSON processed: type=%s state=%s",
                         type->valuestring, state_text);
                cJSON_Delete(root);
                return;
            }
            s_state = XIAOZHI_VOICE_TTS_BUFFERING;
            s_followup_deadline_us = 0;
            s_response_deadline_us = 0;
            if (response_turn)
            {
                s_boot_greeting_reply_deadline_us = 0;
                ESP_LOGI(TAG, "boot greeting direct reply timer paused for cloud response");
            }
            s_boot_greeting_tts_deadline_us = 0;
            s_tts_start_us = esp_timer_get_time();
            s_tts_last_packet_us = 0;
            s_tts_stall_deadline_us = 0;
            s_tts_first_packet_seen = false;
            s_tts_first_pcm_seen = false;
            if (boot_greeting_turn)
            {
                ESP_LOGI(TAG, "boot greeting tts start received: id=%u", (unsigned)s_playing_turn_id);
            }
            if (exit_turn || timeout_turn || fallback_turn)
            {
                s_exit_tts_start_deadline_us = 0;
                s_exit_tts_first_packet_deadline_us = esp_timer_get_time() +
                                                      XIAOZHI_EXIT_TTS_FIRST_PACKET_TIMEOUT_US;
                s_exit_tts_playback_deadline_us = esp_timer_get_time() +
                                                  XIAOZHI_EXIT_TTS_PLAYBACK_TIMEOUT_US;
                ESP_LOGI(TAG, "%s TTS start received: turn=%u; waiting for first binary packet",
                         exit_turn ? "exit" : (timeout_turn ? "timeout prompt" : "fallback prompt"),
                         (unsigned)s_playing_turn_id);
            }
            ESP_LOGI(TAG, "cloud TTS accepted: turn=%u epoch=%u; waiting for PCM prebuffer",
                     (unsigned)s_playing_turn_id, (unsigned)s_tts_playback_epoch);
            if (greeting_turn)
            {
                /* Keep the WSS turn in detect/greeting until playback has
                 * drained.  Opening listen/start here used to consume a
                 * silence primer while the user could already be speaking;
                 * the post-playback VAD handoff now starts the real capture. */
                ESP_LOGI(TAG, "wake greeting TTS accepted: turn=%u; listen/start deferred until PCM drain",
                         (unsigned)s_playing_turn_id);
            }
            if (boot_greeting_turn)
            {
                ESP_LOGI(TAG, "boot greeting TTS accepted: id=%u", (unsigned)s_playing_turn_id);
            }
            if (response_turn)
            {
                publish_ui_state(VOICE_UI_THINKING);
            }
            else
            {
                ESP_LOGI(TAG,
                         "cloud greeting TTS buffering: turn=%u boot=%d wake=%d; waiting for PCM playback",
                         (unsigned)s_playing_turn_id,
                         boot_greeting_turn ? 1 : 0,
                         greeting_turn ? 1 : 0);
            }
        }
        else if (cJSON_IsString(state) && strcmp(state->valuestring, "stop") == 0)
        {
            if (s_discard_tts_stream)
            {
                s_discard_tts_stream = false;
                ESP_LOGI(TAG, "rejected TTS stream ended");
                ESP_LOGI(TAG, "cloud JSON processed: type=%s state=%s",
                         type->valuestring, state_text);
                cJSON_Delete(root);
                return;
            }
            if (s_state != XIAOZHI_VOICE_TTS_BUFFERING &&
                s_state != XIAOZHI_VOICE_SPEAKING)
            {
                ESP_LOGI(TAG, "stale cloud TTS stop ignored in state=%d", s_state);
                ESP_LOGI(TAG, "cloud JSON processed: type=%s state=%s",
                         type->valuestring, state_text);
                cJSON_Delete(root);
                return;
            }
            if ((s_playing_exit_prompt || s_playing_timeout_prompt || s_playing_fallback_prompt) &&
                !s_tts_first_packet_seen)
            {
                ESP_LOGW(TAG, "prompt TTS stopped before binary audio: stage=prompt_tts_no_audio turn=%u",
                         (unsigned)s_playing_turn_id);
                close_channel(false);
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
    }
    else if (strcmp(type->valuestring, "alert") == 0)
    {
        const cJSON *alert_message = cJSON_GetObjectItemCaseSensitive(root, "message");
        const char *detail = cJSON_IsString(alert_message) && alert_message->valuestring != NULL
                                 ? alert_message->valuestring
                                 : "cloud alert";
        ESP_LOGW(TAG, "cloud alert: %s", detail);
        if (s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_TTS)
        {
            finish_boot_greeting_failure(detail);
            ESP_LOGI(TAG, "boot greeting rejected by cloud; ordinary wake word remains available");
            ESP_LOGI(TAG, "cloud JSON processed: type=%s state=%s", type->valuestring, state_text);
            cJSON_Delete(root);
            return;
        }
    }
    else if (strcmp(type->valuestring, "error") == 0)
    {
        if (s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_TTS ||
            s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY || s_playing_boot_greeting)
        {
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
    if (destination == NULL || length == NULL || s_uplink_queue == NULL)
        return false;
    size_t item_size = 0U;
    if (s_uplink_queue_lock != NULL)
        xSemaphoreTake(s_uplink_queue_lock, portMAX_DELAY);
    uint8_t *item = xRingbufferReceive(s_uplink_queue, &item_size, 0);
    if (item != NULL)
    {
        if (item_size <= XIAOZHI_MAX_OPUS_FRAME_BYTES)
        {
            memcpy(destination, item, item_size);
            *length = item_size;
        }
        vRingbufferReturnItem(s_uplink_queue, item);
    }
    if (s_uplink_queue_lock != NULL)
        xSemaphoreGive(s_uplink_queue_lock);
    return item != NULL && *length > 0U;
}

static void report_uplink_failed(void)
{
    if (s_uplink_failed)
        return;
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
    if (s_uplink_frame_buffer == NULL)
    {
        ESP_LOGE(TAG, "voice uplink TX buffer allocation failed");
        report_uplink_failed();
        vTaskSuspend(NULL);
        return;
    }
    for (;;)
    {
        if (!s_uplink_enabled && !s_uplink_primer_active)
        {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }

        const bool primer = s_uplink_primer_active;

        if (s_next_uplink_deadline_us != 0)
        {
            const int64_t remaining_us = s_next_uplink_deadline_us - esp_timer_get_time();
            if (remaining_us > 0)
            {
                const uint32_t delay_ms = (uint32_t)(((remaining_us + 9999LL) / 10000LL) * 10U);
                (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms > 10U ? 10U : delay_ms));
                continue;
            }
        }

        /* Reserve the terminal boundary before touching the queue. If VAD_END
         * wins after this admission, the terminal owner waits for this one
         * already-admitted write instead of silently losing the dequeued tail. */
        if (!begin_main_uplink_send())
            continue;
        size_t frame_len = 0U;
        if (!receive_uplink_frame(s_uplink_frame_buffer, &frame_len))
        {
            (void)finish_main_uplink_send();
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
            continue;
        }
        if (frame_len > XIAOZHI_MAX_OPUS_FRAME_BYTES)
        {
            (void)finish_main_uplink_send();
            continue;
        }
        if (!send_uplink_packet(s_uplink_frame_buffer, frame_len, "bin-main"))
        {
            const bool terminal_deadline = finish_main_uplink_send();
            if (terminal_deadline || s_uplink_terminal_active)
            {
                s_uplink_failed = true;
                ESP_LOGW(TAG, "terminal main write failed in terminal path: turn=%u terminal_gen=%u deadline=%d; terminal owner will end turn",
                         (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation,
                         terminal_deadline ? 1 : 0);
            }
            else
            {
                report_uplink_failed();
            }
            if (terminal_deadline)
            {
                ESP_LOGW(TAG, "terminal main write completed after deadline: turn=%u terminal_gen=%u; ending turn safely",
                         (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation);
            }
            continue;
        }
        const bool terminal_deadline = finish_main_uplink_send();
        if (terminal_deadline)
        {
            ESP_LOGW(TAG, "terminal main write completed after deadline: turn=%u terminal_gen=%u; ending turn safely",
                     (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation);
            continue;
        }
        advance_uplink_pacing();
        log_uplink_traffic(frame_len);
        if (primer && s_uplink_primer_frames_remaining > 0U)
            --s_uplink_primer_frames_remaining;
        if (primer && s_uplink_primer_frames_remaining == 0U)
        {
            s_uplink_primer_active = false;
            reset_uplink_pacing();
            (void)post_event(VOICE_EVENT_WAKE_PRIMER_DONE);
            xTaskNotifyGive(s_session_task);
        }
    }
}

/* ------------------------- Timer chime support ------------------------- */

static void publish_timer_snapshot(void)
{
    timer_service_snapshot_t source;
    timer_service_get_snapshot(&source);
    timer_snapshot_t snapshot = {
        .alarm_active = source.alarm_active,
        .alarm_hour = source.alarm_hour,
        .alarm_minute = source.alarm_minute,
        .countdown_active = source.countdown_active,
        .countdown_remaining_seconds = source.countdown_remaining_seconds,
        .stopwatch_running = source.stopwatch_running,
        .stopwatch_seconds = source.stopwatch_seconds,
        .ringing = source.ringing,
    };
    mid_snapshot_set_timer(&snapshot);
}

static void on_timer_event(timer_service_event_t event, timer_service_slot_t slot)
{
    if (event == TIMER_SERVICE_EVENT_RING)
    {
        post_event_value(VOICE_EVENT_TIMER_RING, (uint32_t)slot);
    }
    else if (event == TIMER_SERVICE_EVENT_STOPPED)
    {
        post_event_value(VOICE_EVENT_TIMER_STOPPED, (uint32_t)slot);
    }
}

/* Plays the local chime through the shared playout.  The FINISHED event is
 * intentionally ignored (the pipeline only forwards TTS playout), so the
 * 2 s timer heartbeat replays the chime while ringing lasts. */
static void begin_timer_chime(void)
{
    if (s_chime_epoch != 0U)
    {
        audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_TIMER_CHIME, s_chime_epoch);
    }
    ++s_chime_epoch;
    const uint32_t epoch = s_chime_epoch;
    const size_t pcm_bytes = TIMER_CHIME_PCM_SAMPLES * sizeof(timer_chime_pcm[0]);
    if (audio_playback_service_begin(AUDIO_PLAYOUT_SOURCE_TIMER_CHIME, epoch, 8U) != ESP_OK)
    {
        ESP_LOGW(TAG, "timer chime begin failed: epoch=%u", (unsigned)epoch);
        return;
    }
    size_t offset = 0U;
    while (offset < pcm_bytes)
    {
        const size_t chunk = (pcm_bytes - offset) > 2048U ? 2048U : (pcm_bytes - offset);
        if (audio_playback_service_enqueue_pcm(
                AUDIO_PLAYOUT_SOURCE_TIMER_CHIME, epoch,
                (const uint8_t *)timer_chime_pcm + offset, chunk) != ESP_OK)
        {
            ESP_LOGW(TAG, "timer chime enqueue failed: epoch=%u offset=%u",
                     (unsigned)epoch, (unsigned)offset);
            audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_TIMER_CHIME, epoch);
            return;
        }
        offset += chunk;
    }
    (void)audio_playback_service_finish(AUDIO_PLAYOUT_SOURCE_TIMER_CHIME, epoch);
    ESP_LOGI(TAG, "timer chime queued: epoch=%u bytes=%u", (unsigned)epoch, (unsigned)pcm_bytes);
}

static void stop_chime(void)
{
    if (s_chime_epoch == 0U)
        return;
    audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_TIMER_CHIME, s_chime_epoch);
    s_chime_epoch = 0U;
    ESP_LOGI(TAG, "timer chime stopped");
}

static void handle_event(voice_event_t *event)
{
    switch (event->type)
    {
    case VOICE_EVENT_WAKE:
        if (s_timeout_ui_active)
        {
            ESP_LOGI(TAG, "wake ignored while local error tone is finishing the turn");
            break;
        }
        if (s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_TTS ||
            s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY || s_playing_boot_greeting)
        {
            ESP_LOGI(TAG, "wake word ignored while boot greeting owns the direct-reply session");
            break;
        }
        if (timer_service_is_ringing())
        {
            ESP_LOGI(TAG, "timer ringing interrupted by wake word; stopping chime");
            timer_service_stop_ringing();
            stop_chime();
            /* pause_detect kept the AFE deaf during the ring; restore it before
             * the follow-up conversation starts below. */
            xiaozhi_audio_pipeline_resume_after_playback();
        }
        if (s_state == XIAOZHI_VOICE_TTS_BUFFERING ||
            s_state == XIAOZHI_VOICE_SPEAKING ||
            s_state == XIAOZHI_VOICE_TTS_DRAINING)
        {
            (void)send_text("{\"type\":\"abort\",\"reason\":\"wake_word_detected\"}");
            ESP_LOGI(TAG, "voice turn abort: id=%u", (unsigned)s_playing_turn_id);
            if (s_pending_countdown_start)
            {
                const timer_service_cmd_result_t result = timer_service_start_pending_countdowns();
                ESP_LOGI(TAG, "deferred countdown finalized after TTS interruption: result=%d",
                         (int)result);
                s_pending_countdown_start = false;
            }
            s_playing_turn_id = 0U;
            s_waiting_turn_id = 0U;
            s_tts_playback_epoch = 0U;
            s_discard_tts_stream = false;
            s_playing_boot_greeting = false;
            s_playing_wake_greeting = false;
            /* Drop prompt bookkeeping so a stale prompt deadline cannot
             * close the fresh turn opened below. */
            s_playing_exit_prompt = false;
            s_exit_prompt_pending = false;
            s_exit_prompt_turn_id = 0U;
            s_playing_timeout_prompt = false;
            s_timeout_prompt_pending = false;
            s_timeout_prompt_turn_id = 0U;
            s_playing_fallback_prompt = false;
            s_fallback_prompt_pending = false;
            s_fallback_prompt_turn_id = 0U;
            s_exit_tts_start_deadline_us = 0;
            s_exit_tts_first_packet_deadline_us = 0;
            s_exit_tts_playback_deadline_us = 0;
            xiaozhi_audio_pipeline_abort_playback();
            /* abort_playback leaves the AFE detection paused; resume it now
             * so the follow-up conversation is not stuck deaf. */
            xiaozhi_audio_pipeline_resume_after_playback();
        }
        if (s_state == XIAOZHI_VOICE_WAIT_HELLO)
            s_pending_wake = true;
        else
            begin_channel();
        break;
    case VOICE_EVENT_VAD_START:
        s_vad_speech = true;
        const uint32_t recent_peak = event->detail;
        if (s_state == XIAOZHI_VOICE_ERROR_TONE)
        {
            ESP_LOGI(TAG, "VAD start ignored while local error tone is finishing the turn");
            break;
        }
        if (s_waiting_for_wake_primer)
        {
            s_wake_primer_candidate_speech = true;
            ESP_LOGI(TAG, "wake primer candidate speech observed; waiting for primer done");
            break;
        }
        if (consume_post_playback_vad_start(recent_peak))
            break;
        if (s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY)
        {
            if (s_wait_for_turn_silence)
            {
                ESP_LOGI(TAG, "boot greeting VAD start ignored; waiting for silence");
                break;
            }
            /* Reject ambient/echo noise that reached VAD_SPEECH but is not
             * loud enough to be the user talking into the mic.  The boot
             * greeting window is a passive listen; without this floor a
             * shelf-mic noise blip (peak 500-1200) opens a cloud turn the
             * user never spoke. */
            if (recent_peak < XIAOZHI_BOOT_GREETING_DIRECT_REPLY_MIN_PEAK)
            {
                ESP_LOGI(TAG, "boot greeting VAD start gated as ambient noise: peak=%u threshold=%u",
                         (unsigned)recent_peak,
                         (unsigned)XIAOZHI_BOOT_GREETING_DIRECT_REPLY_MIN_PEAK);
                break;
            }
            s_boot_greeting_reply_deadline_us = 0;
            s_state = XIAOZHI_VOICE_WAIT_SPEECH;
            start_listen(true);
            ESP_LOGI(TAG, "boot greeting direct reply detected; timer paused, starting cloud turn");
        }
        else if (s_state == XIAOZHI_VOICE_WAIT_FOLLOWUP)
        {
            if (s_wait_for_turn_silence)
            {
                ESP_LOGI(TAG, "follow-up VAD start ignored; waiting for silence");
                break;
            }
            start_followup_listen();
            s_followup_deadline_us = 0;
            ESP_LOGI(TAG, "follow-up speech detected; starting next cloud turn");
        }
        else if (s_state == XIAOZHI_VOICE_WAIT_SPEECH && s_wait_for_turn_silence)
        {
            ESP_LOGI(TAG, "wake-word VAD start ignored; waiting for silence boundary");
        }
        else
        {
            start_listen(true);
        }
        break;
    case VOICE_EVENT_VAD_END:
        s_vad_speech = false;
        if (s_waiting_for_wake_primer)
        {
            /* Keep this latch through the end edge. During a manual wake,
             * speech can finish while the server ignore primer is draining;
             * clearing it here loses the only evidence needed to start the
             * first follow-up capture when the primer completes. */
            ESP_LOGI(TAG, "wake primer candidate speech retained at VAD end: pending=%d",
                     s_wake_primer_candidate_speech ? 1 : 0);
            break;
        }
        if (s_state == XIAOZHI_VOICE_TTS_BUFFERING ||
            s_state == XIAOZHI_VOICE_SPEAKING ||
            s_state == XIAOZHI_VOICE_TTS_DRAINING)
        {
            ESP_LOGI(TAG, "wake-word VAD end ignored while TTS is speaking");
            break;
        }
        if (s_wait_for_turn_silence)
        {
            s_wait_for_turn_silence = false;
            s_post_playback_silence_deadline_us = 0;
            xiaozhi_audio_pipeline_reset_after_silence();
            ESP_LOGI(TAG, "post-playback silence observed; ready for question");
            ESP_LOGI(TAG, "voice turn silence boundary observed; ready for question");
            break;
        }
        if (s_state == XIAOZHI_VOICE_LISTENING)
            finish_listen_with_tail_flush();
        break;
    case VOICE_EVENT_CAPTURE_FINISHED:
        if (event->value >= XIAOZHI_CAPTURE_LOW_LEVEL_RMS)
            break;
        if (s_state != XIAOZHI_VOICE_STOPPING && s_state != XIAOZHI_VOICE_WAIT_RESPONSE)
        {
            ESP_LOGI(TAG, "capture diagnostics ignored outside response path: state=%d rms=%u peak=%u",
                     s_state, (unsigned)event->value, (unsigned)event->detail);
            break;
        }
        s_capture_low_level_turn_id = s_turn_id;
        s_capture_low_level_rms = event->value;
        s_capture_low_level_peak = event->detail;
        ESP_LOGW(TAG, "capture_low_level marked for early retry: turn=%u rms=%u peak=%u threshold=%u",
                 (unsigned)s_capture_low_level_turn_id, (unsigned)s_capture_low_level_rms,
                 (unsigned)s_capture_low_level_peak, (unsigned)XIAOZHI_CAPTURE_LOW_LEVEL_RMS);
        handle_capture_low_level_if_ready();
        break;
    case VOICE_EVENT_NETWORK_LOST:
        if (s_state == XIAOZHI_VOICE_ERROR_TONE)
        {
            ESP_LOGI(TAG, "network lost ignored while local error tone is finishing the turn");
            break;
        }
        close_channel(false);
        break;
    case VOICE_EVENT_WSS_CONNECTED:
        if (event->value != s_wss_connection_seq)
        {
            ESP_LOGI(TAG, "stale WSS connected event ignored: event_seq=%u current_seq=%u",
                     (unsigned)event->value, (unsigned)s_wss_connection_seq);
            break;
        }
        if (s_state == XIAOZHI_VOICE_WAIT_HELLO)
        {
            const char *hello = "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\",\"features\":{\"mcp\":true},\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}";
            if (!send_ws_text_raw(hello))
            {
                ESP_LOGW(TAG, "WSS hello send failed; ending current connection");
                close_channel(false);
            }
        }
        break;
    case VOICE_EVENT_WSS_DISCONNECTED:
        if (event->value != s_wss_connection_seq)
        {
            ESP_LOGI(TAG, "stale WSS disconnect ignored: event_seq=%u current_seq=%u",
                     (unsigned)event->value, (unsigned)s_wss_connection_seq);
            break;
        }
        if (s_client == NULL && !s_session_active && !s_hello_ready &&
            s_state == XIAOZHI_VOICE_WAIT_WAKE)
        {
            ESP_LOGI(TAG, "duplicate WSS disconnect ignored: seq=%u idle already",
                     (unsigned)event->value);
            break;
        }
        if (s_state == XIAOZHI_VOICE_ERROR_TONE)
        {
            ESP_LOGI(TAG, "WSS disconnect ignored while local error tone is finishing the turn");
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
        if (s_session_active)
        {
            /* The user was mid-conversation when the server dropped us:
             * answer with the local error tone instead of bouncing back to
             * the main page silently. */
            ESP_LOGW(TAG, "WSS dropped during an active turn; playing local error tone");
            play_error_tone_then_close(false);
        }
        else
        {
            /* Idle/prewarm link loss: nothing the user asked for was lost. */
            close_channel(false);
        }
        break;
    case VOICE_EVENT_WSS_TEXT:
        if (event->value != s_wss_connection_seq)
        {
            ESP_LOGI(TAG, "stale WSS text ignored: event_seq=%u current_seq=%u",
                     (unsigned)event->value, (unsigned)s_wss_connection_seq);
            break;
        }
        if (s_state == XIAOZHI_VOICE_ERROR_TONE)
        {
            ESP_LOGI(TAG, "WSS text ignored while local error tone is finishing the turn");
            break;
        }
        process_text(event->payload, event->payload_len);
        break;
    case VOICE_EVENT_WSS_BINARY:
        if (s_discard_tts_stream ||
            (s_state != XIAOZHI_VOICE_TTS_BUFFERING && s_state != XIAOZHI_VOICE_SPEAKING) ||
            event->value != s_wss_connection_seq ||
            s_playing_turn_id == 0U)
        {
            ESP_LOGW(TAG, "late TTS packet discarded outside accepted speaking turn: state=%d turn=%u bytes=%u event_seq=%u current_seq=%u",
                     s_state, (unsigned)s_playing_turn_id, (unsigned)event->payload_len,
                     (unsigned)event->value, (unsigned)s_wss_connection_seq);
            break;
        }
        {
            const int64_t now_us = esp_timer_get_time();
            if (event->payload_len < 5U)
            {
                ++s_tts_short_packet_count;
                ESP_LOGW(TAG, "TTS short binary packet: turn=%u bytes=%u count=%u",
                         (unsigned)s_playing_turn_id, (unsigned)event->payload_len,
                         (unsigned)s_tts_short_packet_count);
                /* Not a real Opus frame (usually the stream tail): never feed
                 * it to the decoder, which rejects it with a decode error. */
                break;
            }
            if (!s_tts_first_packet_seen)
            {
                s_tts_first_packet_seen = true;
                if (s_playing_exit_prompt || s_playing_timeout_prompt || s_playing_fallback_prompt)
                {
                    s_exit_tts_first_packet_deadline_us = 0;
                }
                ESP_LOGI(TAG, "tts first binary latency: turn=%u delay_us=%lld",
                         (unsigned)s_playing_turn_id, (long long)(now_us - s_tts_start_us));
            }
            if (s_tts_packet_log_us == 0 || now_us - s_tts_packet_log_us >= XIAOZHI_TRAFFIC_LOG_INTERVAL_US)
            {
                ESP_LOGI(TAG, "TTS packet arrival: turn=%u bytes=%u interval_us=%lld",
                         (unsigned)s_playing_turn_id, (unsigned)event->payload_len,
                         (long long)(s_tts_last_packet_us == 0 ? 0 : now_us - s_tts_last_packet_us));
                s_tts_packet_log_us = now_us;
            }
            s_tts_last_packet_us = now_us;
            s_tts_stall_deadline_us = 0;
        }
        if (xiaozhi_audio_pipeline_enqueue_tts_opus(event->payload, event->payload_len) != ESP_OK)
        {
            ++s_tts_packet_drop_count;
            ESP_LOGW(TAG, "TTS packet dropped: playback queue/result error count=%u",
                     (unsigned)s_tts_packet_drop_count);
        }
        else
        {
            log_tts_traffic(event->payload_len);
        }
        break;
    case VOICE_EVENT_BOOT_GREETING:
        begin_boot_greeting();
        break;
    case VOICE_EVENT_TTS_PLAYBACK_STARTED:
        if (event->value != s_tts_playback_epoch ||
            (s_state != XIAOZHI_VOICE_TTS_BUFFERING && s_state != XIAOZHI_VOICE_TTS_DRAINING))
        {
            ESP_LOGI(TAG, "stale TTS playout start ignored: epoch=%u state=%d",
                     (unsigned)event->value, s_state);
            break;
        }
        if (s_state == XIAOZHI_VOICE_TTS_BUFFERING)
            s_state = XIAOZHI_VOICE_SPEAKING;
        if (s_playing_exit_prompt || s_playing_timeout_prompt || s_playing_fallback_prompt)
        {
            s_exit_tts_playback_deadline_us = esp_timer_get_time() +
                                              XIAOZHI_EXIT_TTS_PLAYBACK_TIMEOUT_US;
            ESP_LOGI(TAG, "prompt TTS first decodable PCM: turn=%u delay_us=%lld",
                     (unsigned)s_playing_turn_id,
                     (long long)(esp_timer_get_time() - s_tts_start_us));
        }
        if (s_playing_boot_greeting && !s_tts_first_pcm_seen)
        {
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
        if (event->value != s_tts_playback_epoch || s_state != XIAOZHI_VOICE_TTS_DRAINING)
        {
            ESP_LOGI(TAG, "stale TTS playout finish ignored: epoch=%u state=%d",
                     (unsigned)event->value, s_state);
            break;
        }
        s_tts_stall_deadline_us = 0;
        if (s_pending_countdown_start)
        {
            const timer_service_cmd_result_t result = timer_service_start_pending_countdowns();
            ESP_LOGI(TAG, "deferred countdown finalized after cloud TTS PCM drain: result=%d",
                     (int)result);
            s_pending_countdown_start = false;
        }
        if (s_pending_radio_play)
        {
            s_pending_radio_play = false;
            ESP_LOGI(TAG, "radio acknowledgement finished; starting radio stream");
            if (radio_stream_service_play_default() != ESP_OK)
            {
                ESP_LOGW(TAG, "radio start failed after acknowledgement");
            }
        }
        enter_followup_after_playback();
        break;
    case VOICE_EVENT_TTS_PLAYBACK_UNDERRUN:
        if (event->value == s_tts_playback_epoch)
        {
            ESP_LOGW(TAG, "TTS playout underrun reported: turn=%u epoch=%u",
                     (unsigned)s_playing_turn_id, (unsigned)event->value);
            if ((s_state == XIAOZHI_VOICE_TTS_BUFFERING ||
                 s_state == XIAOZHI_VOICE_SPEAKING) &&
                s_tts_first_packet_seen && s_tts_stall_deadline_us == 0)
            {
                s_tts_stall_deadline_us = esp_timer_get_time() +
                                          XIAOZHI_TTS_STALL_TIMEOUT_US;
                ESP_LOGW(TAG, "TTS stall watchdog armed; local finish if the stream stays silent for 8 s");
            }
        }
        break;
    case VOICE_EVENT_ERROR_TONE_FINISHED:
        if (event->value != s_error_tone_epoch || s_state != XIAOZHI_VOICE_ERROR_TONE)
        {
            ESP_LOGI(TAG, "stale error tone finish ignored: epoch=%u state=%d",
                     (unsigned)event->value, s_state);
            break;
        }
        s_error_tone_deadline_us = 0;
        ESP_LOGI(TAG, "local error tone finished; closing the voice session");
        close_channel(false);
        break;
    case VOICE_EVENT_UPLINK_FAILED:
        /* A failed uplink write can arrive while the turn is in any live
         * phase (speech, waiting for the cloud answer, or already streaming
         * TTS).  Without an explicit teardown the voice service stays parked
         * in that state and the device appears hung: the WSS link is already
         * marked FAILED by the sender, so session continuity is impossible.
         * Tear down any active session so the caller hears the local error
         * tone and the main page returns, instead of silently freezing. */
        if (s_state == XIAOZHI_VOICE_STOPPING ||
            s_state == XIAOZHI_VOICE_LISTENING ||
            s_state == XIAOZHI_VOICE_WAIT_SPEECH ||
            s_state == XIAOZHI_VOICE_WAIT_RESPONSE ||
            s_state == XIAOZHI_VOICE_TTS_BUFFERING ||
            s_state == XIAOZHI_VOICE_SPEAKING ||
            s_state == XIAOZHI_VOICE_TTS_DRAINING ||
            s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_TTS ||
            s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY ||
            s_waiting_for_wake_primer)
        {
            ESP_LOGW(TAG, "voice turn transport failure; no cloud fallback, entering bounded cleanup: turn=%u terminal_gen=%u state=%d",
                     (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation,
                     (int)s_state);
            play_error_tone_then_close(false);
        }
        else
        {
            ESP_LOGW(TAG, "voice uplink failed in idle state=%d; closing channel", (int)s_state);
            close_channel(false);
        }
        break;
    case VOICE_EVENT_WAKE_PRIMER_DONE:
        if (s_waiting_for_wake_primer)
        {
            s_waiting_for_wake_primer = false;
            s_wake_primer_deadline_us = 0;
            ESP_LOGI(TAG, "wake server ignore primer done: frames=%u turn=%u",
                     (unsigned)XIAOZHI_WAKE_PRIMER_FRAMES, (unsigned)s_turn_id);
            if (s_state == XIAOZHI_VOICE_TTS_BUFFERING ||
                s_state == XIAOZHI_VOICE_SPEAKING ||
                s_state == XIAOZHI_VOICE_TTS_DRAINING)
            {
                break;
            }
            if (s_wake_primer_candidate_speech || s_vad_speech)
            {
                s_wake_primer_candidate_speech = false;
                s_wait_for_turn_silence = false;
                start_listen(true);
            }
            else
            {
                s_wait_for_turn_silence = false;
                s_state = XIAOZHI_VOICE_WAIT_FOLLOWUP;
                s_followup_deadline_us = esp_timer_get_time() + XIAOZHI_FOLLOWUP_WINDOW_US;
            }
        }
        break;
    case VOICE_EVENT_TIMER_RING:
        /* The timer owns the speaker: interrupt any active voice session. */
        if (s_session_active)
        {
            if (s_state == XIAOZHI_VOICE_TTS_BUFFERING ||
                s_state == XIAOZHI_VOICE_SPEAKING ||
                s_state == XIAOZHI_VOICE_TTS_DRAINING)
            {
                (void)send_text("{\"type\":\"abort\",\"reason\":\"timer_interrupt\"}");
            }
            close_channel(false);
        }
        /* Keep WakeNet armed but silence VAD while the chime plays. */
        xiaozhi_audio_pipeline_pause_detect();
        begin_timer_chime();
        break;
    case VOICE_EVENT_TIMER_STOPPED:
        stop_chime();
        /* Restore normal detection unless a fresh session already resumed it. */
        if (!s_session_active || s_state == XIAOZHI_VOICE_WAIT_WAKE)
        {
            xiaozhi_audio_pipeline_resume_after_playback();
        }
        break;
    }
}

static void voice_session_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        voice_event_t event = {0};
        if (xQueueReceive(s_event_queue, &event, pdMS_TO_TICKS(20)) == pdTRUE)
        {
            handle_event(&event);
            free_event(&event);
        }
        if (s_ui_idle_deadline_us != 0 && esp_timer_get_time() >= s_ui_idle_deadline_us)
        {
            s_ui_idle_deadline_us = 0;
            if (s_state == XIAOZHI_VOICE_WAIT_WAKE)
            {
                s_session_active = false;
                publish_ui_state(VOICE_UI_IDLE);
                if (s_client == NULL && xiaozhi_activation_service_is_activated())
                {
                    s_prewarm_retry_delay_us = 0; /* fresh schedule: reset the backoff ramp */
                    s_prewarm_retry_us = esp_timer_get_time() + XIAOZHI_PREWARM_RETRY_US;
                }
            }
        }
        if (s_wait_for_turn_silence && s_post_playback_silence_deadline_us != 0 &&
            esp_timer_get_time() >= s_post_playback_silence_deadline_us)
        {
            s_wait_for_turn_silence = false;
            s_post_playback_silence_deadline_us = 0;
            xiaozhi_audio_pipeline_reset_after_silence();
            ESP_LOGI(TAG, "post-playback silence gate elapsed; ready for question");
        }
        if (s_state == XIAOZHI_VOICE_WAIT_HELLO && s_hello_deadline_us != 0 &&
            esp_timer_get_time() >= s_hello_deadline_us)
        {
            ESP_LOGW(TAG, "cloud hello timeout");
            close_channel(false);
        }
        if (s_waiting_for_wake_primer && s_wake_primer_deadline_us != 0 &&
            esp_timer_get_time() >= s_wake_primer_deadline_us)
        {
            ESP_LOGW(TAG, "wake greeting data timeout: elapsed_ms=10000 limit_ms=10000; keeping WSS alive");
            s_waiting_for_wake_primer = false;
            s_wake_primer_deadline_us = 0;
            s_uplink_primer_active = false;
            s_uplink_primer_frames_remaining = 0U;
            (void)discard_uplink_queue();
            xiaozhi_audio_pipeline_reset_after_silence();
            if (s_state != XIAOZHI_VOICE_TTS_BUFFERING &&
                s_state != XIAOZHI_VOICE_SPEAKING &&
                s_state != XIAOZHI_VOICE_TTS_DRAINING)
            {
                s_state = XIAOZHI_VOICE_WAIT_FOLLOWUP;
                s_followup_deadline_us = esp_timer_get_time() + XIAOZHI_FOLLOWUP_WINDOW_US;
                ESP_LOGI(TAG, "wake primer timeout fallback: follow-up window=15s");
            }
        }
        if (s_state == XIAOZHI_VOICE_WAIT_RESPONSE &&
            s_response_deadline_us != 0 &&
            esp_timer_get_time() >= s_response_deadline_us)
        {
            s_response_deadline_us = 0;
            s_wait_for_turn_silence = false;
            xiaozhi_audio_pipeline_reset_after_silence();
            ESP_LOGW(TAG, "cloud data acquisition timeout; requesting generated timeout prompt");
            if (!request_cloud_timeout_prompt())
            {
                /* Answer the failed turn with the local error tone and close
                 * the session after it finishes instead of bouncing back to
                 * the main page silently. */
                play_error_tone_then_close(true);
            }
        }
        if (s_state == XIAOZHI_VOICE_ERROR_TONE && s_error_tone_deadline_us != 0 &&
            esp_timer_get_time() >= s_error_tone_deadline_us)
        {
            s_error_tone_deadline_us = 0;
            ESP_LOGW(TAG, "local error tone watchdog expired; closing the voice session");
            close_channel(false);
        }
        if (s_state == XIAOZHI_VOICE_EXITING || s_playing_exit_prompt ||
            s_playing_timeout_prompt || s_playing_fallback_prompt)
        {
            const int64_t now_us = esp_timer_get_time();
            const char *stage = NULL;
            int64_t deadline_us = 0;
            if (s_exit_tts_start_deadline_us != 0)
            {
                stage = "exit_tts_start_timeout";
                deadline_us = s_exit_tts_start_deadline_us;
            }
            else if (s_exit_tts_first_packet_deadline_us != 0)
            {
                stage = "exit_tts_first_packet_timeout";
                deadline_us = s_exit_tts_first_packet_deadline_us;
            }
            else if (s_exit_tts_playback_deadline_us != 0)
            {
                stage = "exit_pcm_playback_timeout";
                deadline_us = s_exit_tts_playback_deadline_us;
            }
            if (stage != NULL && now_us >= deadline_us)
            {
                const bool exit_timed_out = s_exit_prompt_pending || s_playing_exit_prompt;
                const bool timeout_timed_out = s_timeout_prompt_pending || s_playing_timeout_prompt;
                const bool fallback_timed_out = s_fallback_prompt_pending || s_playing_fallback_prompt;
                ESP_LOGW(TAG, "cloud prompt timeout: stage=%s turn=%u; retrying once then playing the local error tone",
                         stage, (unsigned)(s_playing_turn_id != 0U ? s_playing_turn_id : (s_exit_prompt_turn_id != 0U ? s_exit_prompt_turn_id : (s_timeout_prompt_turn_id != 0U ? s_timeout_prompt_turn_id : s_fallback_prompt_turn_id))));
                s_exit_tts_start_deadline_us = 0;
                s_exit_tts_first_packet_deadline_us = 0;
                s_exit_tts_playback_deadline_us = 0;
                /* Clear the timed-out prompt slot before retrying so a stale
                 * deadline can never close the fresh turn. */
                s_exit_prompt_pending = false;
                s_exit_prompt_turn_id = 0U;
                s_playing_exit_prompt = false;
                s_timeout_prompt_pending = false;
                s_timeout_prompt_turn_id = 0U;
                s_playing_timeout_prompt = false;
                s_fallback_prompt_pending = false;
                s_fallback_prompt_turn_id = 0U;
                s_playing_fallback_prompt = false;
                /* Stop any stale prompt audio before the retry so the fresh
                 * TTS stream can never mix with the timed-out one. */
                s_discard_tts_stream = true;
                s_tts_playback_epoch = 0U;
                xiaozhi_audio_pipeline_abort_playback();
                bool retried = false;
                if (exit_timed_out && !s_exit_prompt_retried)
                {
                    s_exit_prompt_retried = true;
                    retried = request_cloud_exit_prompt();
                }
                else if (timeout_timed_out && !s_timeout_prompt_retried)
                {
                    s_timeout_prompt_retried = true;
                    retried = request_cloud_timeout_prompt();
                }
                else if (fallback_timed_out && !s_fallback_prompt_retried)
                {
                    s_fallback_prompt_retried = true;
                    retried = request_cloud_fallback_prompt();
                }
                if (!retried)
                {
                    /* Answer the timed-out turn with the local error tone and
                     * close the session after it finishes instead of bouncing
                     * back to the main page silently. */
                    play_error_tone_then_close(true);
                }
            }
        }
        if (s_state == XIAOZHI_VOICE_WAIT_FOLLOWUP && s_followup_deadline_us != 0 &&
            esp_timer_get_time() >= s_followup_deadline_us)
        {
            s_followup_deadline_us = 0;
            ESP_LOGI(TAG, "follow-up window expired; requesting generated exit prompt");
            if (!request_cloud_exit_prompt())
                play_error_tone_then_close(false);
        }
        /* TTS stall watchdog: the server stopped sending audio without a tts
         * stop frame (server-side turn loss).  Finish playback locally so the
         * follow-up/direct-reply window opens instead of hanging until the
         * WSS heartbeat timeout kills the session. */
        if (s_tts_stall_deadline_us != 0 &&
            (s_state == XIAOZHI_VOICE_TTS_BUFFERING ||
             s_state == XIAOZHI_VOICE_SPEAKING) &&
            esp_timer_get_time() >= s_tts_stall_deadline_us)
        {
            s_tts_stall_deadline_us = 0;
            ESP_LOGW(TAG, "TTS stream stalled; finishing playback locally");
            xiaozhi_audio_pipeline_finish_playback();
            s_state = XIAOZHI_VOICE_TTS_DRAINING;
        }
        if (s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_TTS &&
            s_boot_greeting_tts_deadline_us != 0 &&
            esp_timer_get_time() >= s_boot_greeting_tts_deadline_us)
        {
            ESP_LOGW(TAG, "boot greeting TTS timeout");
            finish_boot_greeting_failure("TTS start timeout");
        }
        if (s_boot_greeting_reply_deadline_us != 0 && s_session_active &&
            s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY &&
            esp_timer_get_time() >= s_boot_greeting_reply_deadline_us)
        {
            s_boot_greeting_reply_deadline_us = 0;
            s_boot_greeting_direct_window_active = false;
            ESP_LOGI(TAG, "boot greeting reply window expired; requesting generated exit prompt");
            if (!request_cloud_exit_prompt())
                play_error_tone_then_close(false);
        }
        if (s_prewarm_retry_us != 0 && esp_timer_get_time() >= s_prewarm_retry_us &&
            s_client == NULL && !s_session_active)
        {
            s_prewarm_retry_us = 0;
            prewarm_wss();
        }
        /* Timer service tick: alarm expiry, countdown progress, ring heartbeat. */
        if (esp_timer_get_time() - s_last_timer_tick_us >= 1000000LL)
        {
            s_last_timer_tick_us = esp_timer_get_time();
            timer_service_poll();
            publish_timer_snapshot();
        }
    }
}

static bool wait_for_prewarmed_wss(void)
{
    const int64_t deadline_us = esp_timer_get_time() + XIAOZHI_PREWARM_READY_TIMEOUT_US;
    while (!s_hello_ready && esp_timer_get_time() < deadline_us)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_hello_ready)
    {
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
    if (event_id == WEBSOCKET_EVENT_CONNECTED)
    {
        s_wss_last_connected_us = esp_timer_get_time();
        s_wss_last_ping_us = 0;
        s_wss_last_pong_us = 0;
        s_wss_last_rx_us = s_wss_last_connected_us;
        s_wss_health = XIAOZHI_WSS_HEALTH_HEALTHY;
        ESP_LOGI(TAG, "WSS heartbeat connected: seq=%u ping_interval_sec=10 pong_timeout_sec=30",
                 (unsigned)connection_seq);
        log_wss_wifi_state();
        post_event_value(VOICE_EVENT_WSS_CONNECTED, connection_seq);
    }
    else if (event_id == WEBSOCKET_EVENT_DATA)
    {
        const esp_websocket_event_data_t *data = (const esp_websocket_event_data_t *)event_data;
        if (data != NULL && data->op_code == WS_TRANSPORT_OPCODES_PONG)
        {
            s_wss_last_ping_us = esp_timer_get_time();
        }
        receive_websocket_payload((const esp_websocket_event_data_t *)event_data, connection_seq);
    }
    else if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_ERROR ||
             event_id == WEBSOCKET_EVENT_CLOSED)
    {
        const esp_websocket_event_data_t *data = (const esp_websocket_event_data_t *)event_data;
        if (data != NULL)
        {
            s_wss_last_error = data->error_handle;
            s_wss_last_close_code = data->close_status_code;
            ESP_LOGW(TAG, "WSS transport diagnostics: error_type=%d tls_esp=%d tls_stack=%d tls_flags=%d handshake=%d sock_errno=%d close_code=%d",
                     data->error_handle.error_type,
                     (int)data->error_handle.esp_tls_last_esp_err,
                     data->error_handle.esp_tls_stack_err,
                     data->error_handle.esp_tls_cert_verify_flags,
                     data->error_handle.esp_ws_handshake_status_code,
                     data->error_handle.esp_transport_sock_errno,
                     data->close_status_code);
            if (data->error_handle.error_type == WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT)
            {
                ESP_LOGW(TAG, "WSS heartbeat timeout: no PONG within 30 seconds");
            }
            log_wss_wifi_state();
        }
        s_wss_health = event_id == WEBSOCKET_EVENT_ERROR
                           ? XIAOZHI_WSS_HEALTH_FAILED
                           : XIAOZHI_WSS_HEALTH_STALE;
        ESP_LOGW(TAG, "WSS disconnect callback kind=%s event_id=%ld connection_seq=%u op=%s terminal_gen=%u",
                 wss_event_name(event_id), (long)event_id, (unsigned)connection_seq, s_uplink_tx_operation,
                 (unsigned)s_uplink_terminal_generation);
        /* A disconnect/error on the current connection must NOT be torn down
         * from inside this callback: the callback runs synchronously within the
         * esp_websocket_client task, and calling stop()/destroy() from there
         * would free the client/config/locks that this very task is still using
         * (the component's stop_wait_task logs "Client cannot be stopped from
         * websocket task", yet destroy() still frees the resources), which is a
         * use-after-free that panics the device (LoadProhibited null deref and
         * spinlock assert seen in the field logs).  Because the WSS client is
         * built with disable_auto_reconnect = true, the websocket task always
         * exits once the link drops, so we only mark it for the task itself to
         * free on exit via destroy_on_exit(), then clear our app-side state so
         * the next start_wss() builds a fresh socket instead of reusing a
         * half-open one whose is_connected() may still return true. */
        if (s_client != NULL && connection_seq == s_wss_connection_seq)
        {
            /* 用带超时的锁，而不是无限等待：上行发送任务可能正持着这把锁卡在
             * send_bin 的写阻塞里（socket 已死）。若回调在此处永久抢锁，就永远
             * 执行不到下面的 post_wss_disconnect_event，断连事件随之丢失，
             * 设备会在"残连假死"连接上永久空转（卡死聆听）。 */
            const bool lock_ok = (s_ws_tx_lock == NULL) ||
                                 (xSemaphoreTake(s_ws_tx_lock, pdMS_TO_TICKS(300)) == pdTRUE);
            if (lock_ok)
            {
                (void)esp_websocket_client_destroy_on_exit(s_client);
                s_client = NULL;
                s_client_started = false;
                s_hello_ready = false;
                s_session_id[0] = '\0';
                if (s_ws_tx_lock != NULL)
                    xSemaphoreGive(s_ws_tx_lock);
            }
            else
            {
                /* 上行发送任务此刻正占用锁并卡在 send_bin。send_bin 的 client
                 * 参数已在函数调用点压栈复制，此处清空全局 s_client 不会影响
                 * 该任务继续执行。若不置空，websocket 任务在 selected_for_destroying
                 * 退出时会释放整个 client（含 status_bits event group），遗留的
                 * 悬空 s_client 会让 close_channel 对它再次 stop()，触发
                 * xEventGroupClearBits 断言（UAF 崩溃重启）。因此这里与抢锁成功
                 * 分支保持一致，同步清空 client 与 started 标志；destroy_on_exit
                 * 打标记由 websocket 任务退出时自清理，并强制健康失效，靠下方
                 * 无条件投递的事件驱动收场。 */
                (void)esp_websocket_client_destroy_on_exit(s_client);
                s_client = NULL;
                s_client_started = false;
                s_wss_health = XIAOZHI_WSS_HEALTH_FAILED;
                ESP_LOGW(TAG, "WSS callback lock timeout (seq=%u); forcing health failed, event will drive teardown",
                         (unsigned)connection_seq);
            }
        }
        /* 断连事件必须始终投递：否则设备无从感知连接已死，会在假死连接上空转，
         * 错过本地错误音的收场反馈。 */
        post_wss_disconnect_event(connection_seq, event_id);
    }
}

static void wake_word_detected(void *context)
{
    (void)context;
    ESP_LOGI(TAG, "WakeNet detected; queuing voice session");
    post_event(VOICE_EVENT_WAKE);
}

static void vad_changed(bool speech, uint32_t recent_peak, void *context)
{
    (void)context;
    (void)post_event_detail(speech ? VOICE_EVENT_VAD_START : VOICE_EVENT_VAD_END,
                            0U, speech ? recent_peak : 0U);
}

static void capture_finished(uint32_t rms, uint32_t peak, uint32_t frames,
                             int64_t duration_us, void *context)
{
    (void)context;
    ESP_LOGI(TAG, "voice capture completed: rms=%u peak=%u frames=%u duration_us=%lld",
             (unsigned)rms, (unsigned)peak, (unsigned)frames, (long long)duration_us);
    (void)post_event_detail(VOICE_EVENT_CAPTURE_FINISHED, rms, peak);
}

static void tts_playback_changed(xiaozhi_tts_playback_event_t event, uint32_t epoch, void *context)
{
    (void)context;
    switch (event)
    {
    case XIAOZHI_TTS_PLAYBACK_STARTED:
        post_playback_event(VOICE_EVENT_TTS_PLAYBACK_STARTED, epoch);
        break;
    case XIAOZHI_TTS_PLAYBACK_FINISHED:
        post_playback_event(VOICE_EVENT_TTS_PLAYBACK_FINISHED, epoch);
        break;
    case XIAOZHI_TTS_PLAYBACK_UNDERRUN:
        post_playback_event(VOICE_EVENT_TTS_PLAYBACK_UNDERRUN, epoch);
        break;
    case XIAOZHI_ERROR_TONE_PLAYBACK_FINISHED:
        post_playback_event(VOICE_EVENT_ERROR_TONE_FINISHED, epoch);
        break;
    }
}

static void voice_pipeline_init_task(void *arg)
{
    TaskHandle_t waiter = (TaskHandle_t)arg;
    s_pipeline_init_result = xiaozhi_audio_pipeline_init(wake_word_detected, vad_changed,
                                                         opus_frame_ready, tts_playback_changed,
                                                         capture_finished, NULL);
    const unsigned watermark = (unsigned)uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "voice pipeline init task result=%s", esp_err_to_name(s_pipeline_init_result));
    ESP_LOGI(TAG, "voice pipeline init internal stack watermark=%u", watermark);
    s_pipeline_init_task = NULL;
    if (waiter != NULL)
        xTaskNotifyGive(waiter);
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
    if (s_pipeline_init_task == NULL)
    {
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
    if (opus == NULL || opus_bytes == 0U || s_uplink_queue == NULL)
        return;
    if (s_uplink_queue_lock != NULL)
        xSemaphoreTake(s_uplink_queue_lock, portMAX_DELAY);
    if (s_uplink_terminal_active)
    {
        ++s_uplink_drop_count;
        if (s_uplink_queue_lock != NULL)
            xSemaphoreGive(s_uplink_queue_lock);
        ESP_LOGI(TAG, "Opus uplink rejected at terminal boundary: turn=%u terminal_gen=%u dropped=%u",
                 (unsigned)s_turn_id, (unsigned)s_uplink_terminal_generation,
                 (unsigned)s_uplink_drop_count);
        return;
    }
    BaseType_t sent = xRingbufferSend(s_uplink_queue, opus, opus_bytes, 0);
    if (sent != pdTRUE)
    {
        size_t dropped_size = 0U;
        void *dropped = xRingbufferReceive(s_uplink_queue, &dropped_size, 0);
        if (dropped != NULL)
            vRingbufferReturnItem(s_uplink_queue, dropped);
        sent = xRingbufferSend(s_uplink_queue, opus, opus_bytes, 0);
        ++s_uplink_drop_count;
        ESP_LOGW(TAG, "Opus uplink queue full; dropped oldest frame and retried result=%d count=%u",
                 (int)sent, (unsigned)s_uplink_drop_count);
    }
    if (s_uplink_queue_lock != NULL)
        xSemaphoreGive(s_uplink_queue_lock);
    if (sent == pdTRUE)
        notify_uplink_task();
}

esp_err_t xiaozhi_voice_service_init(void)
{
    if (s_initialized)
        return ESP_OK;
    s_event_queue = xQueueCreate(XIAOZHI_EVENT_QUEUE_DEPTH, sizeof(voice_event_t));
    s_uplink_queue = xRingbufferCreateWithCaps(XIAOZHI_UPLINK_QUEUE_BYTES, RINGBUF_TYPE_NOSPLIT,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_uplink_queue_lock = xSemaphoreCreateMutex();
    s_ws_tx_lock = xSemaphoreCreateMutex();
    if (s_event_queue == NULL || s_uplink_queue == NULL || s_uplink_queue_lock == NULL ||
        s_ws_tx_lock == NULL)
    {
        cleanup_init_failure();
        return ESP_ERR_NO_MEM;
    }
    s_uplink_frame_buffer = heap_caps_malloc(XIAOZHI_MAX_OPUS_FRAME_BYTES,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_uplink_frame_buffer == NULL)
    {
        cleanup_init_failure();
        return ESP_ERR_NO_MEM;
    }
    /* ESP-WebSocket creates a fixed 4 KiB internal-RAM task. Reserve it
     * before AFE/WakeNet allocates and fragments the remaining internal heap. */
    s_initialized = true;
    publish_ui_state(VOICE_UI_IDLE);
    /* Timer service restores persisted alarms/countdowns and forwards ring
     * events into this task's queue. */
    (void)timer_service_init(on_timer_event);
    publish_timer_snapshot();
    if (xiaozhi_audio_pipeline_prepare_encoder() != ESP_OK)
    {
        cleanup_init_failure();
        return ESP_ERR_NO_MEM;
    }
    prewarm_wss();
    /* The WSS callback posts CONNECTED/DATA events into s_event_queue. Create
     * its consumer after the socket task exists but before waiting for hello. */
    if (xTaskCreatePinnedToCoreWithCaps(voice_session_task, "voice_session", 6144, NULL, 3,
                                        &s_session_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    {
        cleanup_init_failure();
        return ESP_ERR_NO_MEM;
    }
    if (!wait_for_prewarmed_wss())
    {
        cleanup_init_failure();
        return ESP_ERR_TIMEOUT;
    }
    if (xTaskCreatePinnedToCoreWithCaps(voice_uplink_tx_task, "voice_uplink_tx", 6144, NULL, 4,
                                        &s_uplink_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    {
        cleanup_init_failure();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = init_audio_pipeline_on_internal_stack();
    if (err != ESP_OK)
    {
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
        s_boot_greeting_request_pending)
        return ESP_ERR_INVALID_STATE;
    s_boot_greeting_request_pending = true;
    if (post_event(VOICE_EVENT_BOOT_GREETING))
        return ESP_OK;
    s_boot_greeting_request_pending = false;
    return ESP_ERR_TIMEOUT;
}

void xiaozhi_voice_service_network_lost(void)
{
    if (!s_initialized)
        return;
    if (!s_session_active && s_state == XIAOZHI_VOICE_WAIT_WAKE && s_client == NULL)
    {
        ESP_LOGI(TAG, "voice network lost ignored: no active session");
        return;
    }
    post_event(VOICE_EVENT_NETWORK_LOST);
}
