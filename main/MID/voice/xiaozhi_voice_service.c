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
#include "mcp_service.h"
#include "radio_stream_service.h"

#define XIAOZHI_EVENT_QUEUE_DEPTH 12U
#define XIAOZHI_UPLINK_QUEUE_BYTES (64U * 1024U)
#define XIAOZHI_TEXT_MAX_BYTES 1024U
#define XIAOZHI_BINARY_MAX_BYTES 2048U
/* An MCP reply is the JSON-RPC object plus this envelope; the widest one is the
 * tools/list answer (its 13-tool catalogue alone is ~2.9 KB), so the frame
 * buffer below is sized with that headroom on top of the session id.  It must
 * stay larger than MCP_REPLY_MAX_BYTES in mcp_service.c. */
#define XIAOZHI_MCP_FRAME_MAX_BYTES 5120U
#define XIAOZHI_HELLO_TIMEOUT_US (8LL * 1000LL * 1000LL)
#define XIAOZHI_PREWARM_READY_TIMEOUT_US (30LL * 1000LL * 1000LL)
#define XIAOZHI_UI_ERROR_HOLD_US (2LL * 1000LL * 1000LL)
#define XIAOZHI_PREWARM_RETRY_US (5LL * 1000LL * 1000LL)
/* 电台在播时用户随时可能打断，等 5 秒等于让每一次唤醒都落在“没有热连接”的窗口
 * 里，只能现建 TLS+握手（实测 1.0~2.5 秒）。电台一开播就先把 WSS 建好并保温，
 * 唤醒时直接复用这条连接。 */
#define XIAOZHI_PREWARM_RADIO_DELAY_US (300LL * 1000LL)
#define XIAOZHI_PREWARM_RETRY_MAX_US (30LL * 1000LL * 1000LL)
/* 电台让位是异步的：yield() 只置位，拉流任务要等自己那一轮循环退出才会关掉 HTTP/TLS
 * 并归还内部 RAM。而 WSS 的 websocket task 要 4096 字节连续内部 RAM，实测电台拉流时
 * 只剩 4631 字节 / 最大连续块 1920（"internal contiguous block below requested
 * stack"），所以唤醒后必须等电台真的停稳再建连，否则每一次"电台在播时唤醒"都直接
 * 吃一个连接失败。上限 300 ms，等不到就照常尝试，不把唤醒拖长。 */
#define XIAOZHI_RADIO_PARK_WAIT_US (300LL * 1000LL)
#define XIAOZHI_TRAFFIC_LOG_INTERVAL_US (1LL * 1000LL * 1000LL)
#define XIAOZHI_OPUS_FRAME_INTERVAL_US (60LL * 1000LL)
#define XIAOZHI_UPLINK_TAIL_FLUSH_FRAMES 2U
#define XIAOZHI_UPLINK_TAIL_FLUSH_TIMEOUT_US (150LL * 1000LL)
#define XIAOZHI_UPLINK_MAIN_WRITE_WAIT_TIMEOUT_US (1200LL * 1000LL)
#define XIAOZHI_WAKE_PRIMER_FRAMES 35U
#define XIAOZHI_WAKE_PRIMER_TIMEOUT_US (10LL * 1000LL * 1000LL)
/* Post-turn listening window.  6.4 s is enough for a speaker who wants
 * another turn to start talking, and a longer hold only keeps the session and
 * its UI alive for no benefit. */
#define XIAOZHI_FOLLOWUP_WINDOW_US (6400LL * 1000LL)
/* The official xiaozhi.me backend can take longer than 10 s before the first
 * frame of a turn arrives; 15 s avoids a false "didn't catch that" timeout
 * while the cloud is merely slow. */
#define XIAOZHI_WAIT_RESPONSE_TIMEOUT_US (15LL * 1000LL * 1000LL)
#define XIAOZHI_EXIT_TTS_START_TIMEOUT_US (20LL * 1000LL * 1000LL)
#define XIAOZHI_EXIT_TTS_FIRST_PACKET_TIMEOUT_US (8LL * 1000LL * 1000LL)
#define XIAOZHI_EXIT_TTS_PLAYBACK_TIMEOUT_US (45LL * 1000LL * 1000LL)
/* Watchdog for the local error tone that answers a failed turn before the
 * session closes; the tone itself is 2 s, so 5 s leaves ample slack. */
#define XIAOZHI_ERROR_TONE_DEADLINE_US (5LL * 1000LL * 1000LL)
/* The local intent confirmation prompt is a very short (< 1.5 s) phrase; the
 * 3 s watchdog only unblocks a queued cloud reply if the playout stalls. */
#define XIAOZHI_LOCAL_PROMPT_DEADLINE_US (3LL * 1000LL * 1000LL)
/* 云端音频到达时，本地提示音只剩这么多就不再硬切，让它把最后两个字说完。
 *
 * 让位走的是 audio_playback_service_abort()：discard_frames() + 直接静音 codec，
 * 没有淡出。切在“正在执”和“行”之间就是一声闷响加半句话。实测云端首个音频包
 * 平均 1.39 s 就位（23 个样本，中位数 1.07 s，最快 0.32 s），而提示音只有 0.95 s：
 * 多数时候它能先放完，但总有一部分场景正巧切在句子中间。
 *
 * 250 ms 是折中：比一个汉字的时长大一点，够把最后一两个字收干净；代价上限也只有
 * 250 ms，且只发生在“贴着尾声到达”这一种情况下。剩余更多时照旧立即让位——
 * 让云端早出声仍然是优先级，这里只是不让它切出一句半截话。 */
#define XIAOZHI_LOCAL_PROMPT_GRACE_US (250LL * 1000LL)
/* 本地提示音统一为 16 kHz 单声道 s16le：每毫秒 32 字节。用来把 PCM 字节数换算成
 * 墙上时间—— s_local_prompt_deadline_us 是 3 秒看门狗，不是音频结束点，两者不能混用。 */
#define XIAOZHI_LOCAL_PROMPT_BYTES_PER_MS 32
/* Embedded session prompts (boot greeting / exit / timeout / fallback) share
 * the same playout.  The boot greeting is the longest: it concatenates four
 * segments (period + spoken clock + weather copy + help offer).  The longest
 * asset of each table measures about 5.2 s combined (0.75 greeting + 1.60
 * clock + 1.36 weather + 1.52 offer).  The assets sit at TTS_RATE = +18% on
 * purpose: at the original rate the four segments dragged and pushed the
 * combined clip past 7.6 s, which sounded sluggish next to the cloud reply.
 * Regenerating the PCMs with scripts/generate_prompt_tones.py is the only way
 * these numbers move, so keep this comment in step with that script.  The
 * total still fits inside the ten-second session watchdog below with room to
 * spare. */
#define XIAOZHI_LOCAL_SESSION_PROMPT_DEADLINE_US (10LL * 1000LL * 1000LL)
#define XIAOZHI_BOOT_GREETING_TTS_TIMEOUT_US (8LL * 1000LL * 1000LL)
/* The boot greeting hands over to exactly the same wait a normal turn uses. */
#define XIAOZHI_BOOT_GREETING_REPLY_WINDOW_US (6400LL * 1000LL)
/* After a playback underrun, if the server sends neither more TTS audio nor
 * a tts stop frame within this window, the stream is declared stalled and
 * playback finishes locally instead of hanging until the WSS heartbeat
 * timeout kills the whole session (server-side turn loss).
 *
 * 实测：云端把一段话拆成多句合成，两句之间最长能停 12.1~12.8 s（整段停顿期间设备
 * 靠 underrun 补静音撑着）。5 s 的窗口撑不到那个洞，会在第二句到达之前就本地收尾，
 * 第二句随后被当作 late TTS packet 丢掉——听感就是“话说到一半没了”，用户把它
 * 描述成“卡顿丢字”。用户明确要求不丢字优先，所以取 15 s 盖住这个已测到的上限。
 * 代价：云端真的丢了这一轮时，用户要等满 15 s 才回到可说话状态；真正死掉的连接
 * 由 WSS 心跳（ping 10 s / pong 20 s）作最后兜底。 */
#define XIAOZHI_TTS_STALL_TIMEOUT_US (15LL * 1000LL * 1000LL)
/* 受理了云端 TTS 却一帧 PCM 都没出声的兜底。停顿看门狗只在“开播后又断流”
   （UNDERRUN）时布防，如果播放根本没开播就永远不布防，而电台还停在“让位保活”
   上，整机会卡在静音+无监听的状态里。实测：云端只发了 13 帧（780 ms）就静了
   12.7 s，开播门槛永远凑不满，串口连开播日志都没有。只要首帧 PCM 到这个
   时限还没出现，就本地收尾，把扬声器还给电台并结束这一轮。 */
#define XIAOZHI_TTS_FIRST_PCM_TIMEOUT_US (10LL * 1000LL * 1000LL)
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
/* The follow-up window is the session's last chance to stay alive, so a
 * spurious VAD start there is worse than anywhere else: every ambient burst
 * opens a fresh cloud turn and re-arms the window, so the device never reaches
 * its follow-up timeout and sits in the listening state forever (observed
 * 1268/2029/2229 peak bursts while the user stayed silent).  The user has just
 * been talking to the device, so real speech is close-mic loud: require that
 * level before the bounded window is consumed.  Anything below it keeps the
 * window running so it can expire and close the session normally. */
#define XIAOZHI_FOLLOWUP_DIRECT_REPLY_MIN_PEAK 2500U
#define XIAOZHI_CAPTURE_LOW_LEVEL_RMS 200U
#define XIAOZHI_MAX_OPUS_FRAME_BYTES 4096U
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
    VOICE_EVENT_LOCAL_PROMPT_FINISHED,
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
/* Local intent confirmation prompt: epoch, active flag and its watchdog.
 * The pending fields hold a cloud TTS start that arrived while the prompt
 * was still playing, so the reply is released on prompt FINISHED instead of
 * being dropped or interrupting the prompt. */
static uint32_t s_local_prompt_epoch;
static int64_t s_local_prompt_deadline_us;
/* 提示音预计播完的墙上时刻。与上面的 3 秒看门狗是两回事：看门狗只防播放卡死，
   这个才是“还剩多少音频没放”，优雅让位靠它判断。 */
static int64_t s_local_prompt_end_us;
static bool s_local_prompt_active;
static bool s_pending_cloud_tts_after_prompt;
static uint32_t s_pending_cloud_tts_turn_id;
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
/* 第一段聆听窗口没等到人说话时，已经播过一次“我没听清，可以再说一遍吗？”并重开。
   第二次到期仍走原来的退出流程——不设这个标记就会无限重开，设备永远不回待机。
   用户在窗口里真正开口（follow-up VAD start 过了峰值门限）或整段会话关闭时复位。 */
static bool s_followup_retry_used;
/* 唤醒那一刻是不是已经是电台模式（在放、或在重连路上）。收场时靠它区分两种
   "电台在播"：
   - 用户打断电台：答完要先把 XIAOZHI_FOLLOWUP_WINDOW_US 的听音窗口留满，
     窗口期间电台继续让位（哑的，不存在自激），让用户能直接接话；
   - 用户这一轮才下的"开电台"命令：他要的就是电台出声，不能压着窗口。
   follow-up 续轮不经过唤醒事件，所以这个标记在整段对话里保持不变，直到
   close_channel() 归还扬声器、或电台在本轮被拉起来时才清掉。 */
static bool s_radio_on_before_turn;
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
static int64_t s_tts_first_pcm_deadline_us;
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
/* Set when the session ends because the exchange was complete: the server closed
 * a delivered turn ("终止对话") or the farewell prompt drained.  The teardown
 * then returns to the main page quietly - a normal exit is not a fault. */
static bool s_session_exit_clean;
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
/* Which prompt the shared AUDIO_PLAYOUT_SOURCE_LOCAL_PROMPT stream currently
 * carries.  The official xiaozhi.me server rejects the [device_call] direct-TTS
 * prefix, so the boot greeting, the exit farewell, the response-timeout hint
 * and the uplink-failure fallback all play from embedded PCM assets; this kind
 * decides what the session does once they drain. */
typedef enum
{
    LOCAL_PROMPT_NONE = 0,
    LOCAL_PROMPT_INTENT,
    LOCAL_PROMPT_BOOT_GREETING,
    LOCAL_PROMPT_EXIT,
    LOCAL_PROMPT_TIMEOUT,
    LOCAL_PROMPT_FALLBACK,
} local_prompt_kind_t;
static local_prompt_kind_t s_local_prompt_kind = LOCAL_PROMPT_NONE;
/* Plays a single embedded PCM prompt through the shared playout.  The exit,
 * timeout and fallback paths call this hundreds of lines before its definition,
 * which sits next to the boot-greeting segment player, so it needs a forward
 * declaration here. */
static bool play_local_prompt_pcm(local_prompt_kind_t kind, const uint8_t *start,
                                  const uint8_t *end, uint32_t *epoch_out);
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
/* MCP replies live here instead of on the session task stack: a tools/list
 * answer can reach ~3 KB and that task only has 6 KB total.  process_text()
 * is the only writer, so one buffer is enough. */
static char s_mcp_tx_frame[XIAOZHI_MCP_FRAME_MAX_BYTES];
static websocket_accumulator_t s_rx;

/* The exit / timeout / fallback wording used to be cloud text wrapped in
 * [device_call].  The official xiaozhi.me server rejects that prefix, so the
 * copy is now frozen into pre-rendered PCM assets generated by
 * scripts/generate_prompt_tones.py:
 *
 *   prompt_exit_16k_mono_s16le.pcm      <- 对话静默窗口到期后的固定告别语
 *   prompt_timeout_16k_mono_s16le.pcm   <- 云端应答超时的提示语
 *   prompt_fallback_16k_mono_s16le.pcm  <- 本地回合失败时的兜底提示语
 *
 * Re-run that script (and rebuild) to change the wording. */

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

/* The official server rejects [device_call], so the farewell is an embedded
 * PCM asset instead of a cloud TTS request.  The function name is kept so the
 * surrounding session state machine and its contracts stay stable. */
static bool request_cloud_exit_prompt(void)
{
    extern const uint8_t prompt_exit_pcm_start[] asm(
        "_binary_prompt_exit_16k_mono_s16le_pcm_start");
    extern const uint8_t prompt_exit_pcm_end[] asm(
        "_binary_prompt_exit_16k_mono_s16le_pcm_end");
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
    s_response_deadline_us = 0;
    /* The original speaking turn is superseded by the local farewell; a late
     * cloud reply for it must not collide with the prompt slot. */
    s_waiting_turn_id = 0U;
    s_exit_tts_start_deadline_us = 0;
    s_exit_tts_first_packet_deadline_us = 0;
    s_exit_tts_playback_deadline_us = 0;
    s_wait_for_turn_silence = false;
    xiaozhi_audio_pipeline_reset_after_silence();
    (void)discard_uplink_queue();
    if (!play_local_prompt_pcm(LOCAL_PROMPT_EXIT, prompt_exit_pcm_start,
                               prompt_exit_pcm_end, NULL))
    {
        s_exit_prompt_pending = false;
        s_exit_prompt_turn_id = 0U;
        ESP_LOGW(TAG, "exit prompt unavailable: local farewell PCM failed to play");
        return false;
    }
    s_exit_prompt_retried = false;
    ESP_LOGI(TAG, "exit prompt selected: local PCM bytes=%u",
             (unsigned)(prompt_exit_pcm_end - prompt_exit_pcm_start));
    ESP_LOGI(TAG, "exit prompt playing: turn=%u", (unsigned)s_exit_prompt_turn_id);
    publish_ui_state(VOICE_UI_THINKING);
    return true;
}

static bool request_cloud_timeout_prompt(void)
{
    extern const uint8_t prompt_timeout_pcm_start[] asm(
        "_binary_prompt_timeout_16k_mono_s16le_pcm_start");
    extern const uint8_t prompt_timeout_pcm_end[] asm(
        "_binary_prompt_timeout_16k_mono_s16le_pcm_end");
    ++s_turn_id;
    if (s_turn_id == 0U)
        ++s_turn_id;
    s_timeout_prompt_turn_id = s_turn_id;
    s_timeout_prompt_pending = true;
    s_state = XIAOZHI_VOICE_EXITING;
    s_response_deadline_us = 0;
    /* The timed-out turn is superseded by the local hint; a late cloud reply
     * for it must not collide with the prompt slot. */
    s_waiting_turn_id = 0U;
    s_exit_prompt_pending = false;
    s_exit_prompt_turn_id = 0U;
    s_fallback_prompt_pending = false;
    s_fallback_prompt_turn_id = 0U;
    s_exit_tts_start_deadline_us = 0;
    s_exit_tts_first_packet_deadline_us = 0;
    s_exit_tts_playback_deadline_us = 0;
    s_wait_for_turn_silence = false;
    xiaozhi_audio_pipeline_reset_after_silence();
    (void)discard_uplink_queue();
    if (!play_local_prompt_pcm(LOCAL_PROMPT_TIMEOUT, prompt_timeout_pcm_start,
                               prompt_timeout_pcm_end, NULL))
    {
        s_timeout_prompt_pending = false;
        s_timeout_prompt_turn_id = 0U;
        ESP_LOGW(TAG, "timeout prompt unavailable: local PCM failed to play; falling back to timeout UI");
        return false;
    }
    s_timeout_prompt_retried = false;
    ESP_LOGI(TAG, "timeout prompt selected: local PCM bytes=%u",
             (unsigned)(prompt_timeout_pcm_end - prompt_timeout_pcm_start));
    ESP_LOGI(TAG, "timeout prompt playing: turn=%u", (unsigned)s_timeout_prompt_turn_id);
    publish_ui_state(VOICE_UI_THINKING);
    return true;
}

static bool request_cloud_fallback_prompt(void)
{
    extern const uint8_t prompt_fallback_pcm_start[] asm(
        "_binary_prompt_fallback_16k_mono_s16le_pcm_start");
    extern const uint8_t prompt_fallback_pcm_end[] asm(
        "_binary_prompt_fallback_16k_mono_s16le_pcm_end");
    ++s_turn_id;
    if (s_turn_id == 0U)
        ++s_turn_id;
    s_fallback_prompt_turn_id = s_turn_id;
    s_fallback_prompt_pending = true;
    s_state = XIAOZHI_VOICE_EXITING;
    s_response_deadline_us = 0;
    /* The failed turn is superseded by the local fallback hint; a late cloud
     * reply for it must not collide with the prompt slot. */
    s_waiting_turn_id = 0U;
    s_exit_prompt_pending = false;
    s_exit_prompt_turn_id = 0U;
    s_timeout_prompt_pending = false;
    s_timeout_prompt_turn_id = 0U;
    s_exit_tts_start_deadline_us = 0;
    s_exit_tts_first_packet_deadline_us = 0;
    s_exit_tts_playback_deadline_us = 0;
    s_wait_for_turn_silence = false;
    xiaozhi_audio_pipeline_reset_after_silence();
    (void)discard_uplink_queue();
    if (!play_local_prompt_pcm(LOCAL_PROMPT_FALLBACK, prompt_fallback_pcm_start,
                               prompt_fallback_pcm_end, NULL))
    {
        s_fallback_prompt_pending = false;
        s_fallback_prompt_turn_id = 0U;
        ESP_LOGW(TAG, "fallback prompt unavailable: local PCM failed to play; ending voice session");
        return false;
    }
    s_fallback_prompt_retried = false;
    ESP_LOGI(TAG, "fallback prompt selected: local PCM bytes=%u",
             (unsigned)(prompt_fallback_pcm_end - prompt_fallback_pcm_start));
    ESP_LOGI(TAG, "fallback prompt playing: turn=%u", (unsigned)s_fallback_prompt_turn_id);
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
    /* 抢扬声器之前先让电台退开：让位和停止不是一回事，电台的整体流程要活着。 */
    (void)radio_stream_service_yield();
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

/* Heuristic that decides which confirmation prompt to play when the utterance
 * is not a local IoT command: query keywords (weather/time/date/temperature)
 * choose "正在查询", anything else is treated as chat and plays "正在思考".
 * The plan accepts the rare mis-hit because it only changes the prompt copy,
 * never the eventual cloud reply. */
static bool is_query_intent(const char *text)
{
    if (text == NULL) return false;
    static const char *const query_keywords[] = {
        "天气", "几点", "时间", "日期", "星期", "温度", "多少度", "预报",
        "今天", "明天", "现在几点", "几点钟", "何时", "多久", "多长时间",
        "几月", "几号", "几时", "下雨", "晴天", "阴天", "下雪", "气温",
        "湿度", "穿衣", "风向",
    };
    for (size_t i = 0U; i < sizeof(query_keywords) / sizeof(query_keywords[0]); ++i) {
        if (strstr(text, query_keywords[i]) != NULL) return true;
    }
    return false;
}

/* One embedded PCM segment.  A composite prompt such as the boot greeting is a
 * sequence of these; the shared playout is a streaming producer/consumer, so
 * concatenating short assets costs flash but no extra RAM. */
typedef struct
{
    const uint8_t *start;
    const uint8_t *end;
} local_pcm_segment_t;

/* Plays an embedded PCM prompt through the shared playout.  Mirrors
 * begin_error_tone() but uses AUDIO_PLAYOUT_SOURCE_LOCAL_PROMPT so its FINISHED
 * is dispatched by kind: an intent prompt releases a queued cloud reply, while
 * the session prompts (boot greeting / exit / timeout / fallback) resume their
 * own part of the state machine.
 *
 * Every segment is enqueued back to back before finish() is called, so the
 * playout task never runs dry between them: audio_playback_service_enqueue_pcm()
 * blocks on the frame queue instead of dropping the tail of a segment. */
static bool play_local_prompt_segments(local_prompt_kind_t kind,
                                       const local_pcm_segment_t *segments,
                                       size_t segment_count, uint32_t *epoch_out)
{
    if (segments == NULL || segment_count == 0U)
    {
        ESP_LOGW(TAG, "local prompt rejected: no segment supplied");
        return false;
    }
    size_t total_bytes = 0U;
    for (size_t index = 0U; index < segment_count; ++index)
    {
        if (segments[index].start == NULL || segments[index].end <= segments[index].start)
        {
            ESP_LOGW(TAG, "local prompt rejected: segment %u is empty", (unsigned)index);
            return false;
        }
        total_bytes += (size_t)(segments[index].end - segments[index].start);
    }
    ++s_local_prompt_epoch;
    if (s_local_prompt_epoch == 0U) ++s_local_prompt_epoch;
    const uint32_t epoch = s_local_prompt_epoch;
    /* 抢扬声器之前先让电台退开：让位和停止不是一回事，电台的整体流程要活着。 */
    (void)radio_stream_service_yield();
    if (audio_playback_service_begin(AUDIO_PLAYOUT_SOURCE_LOCAL_PROMPT, epoch, 8U) != ESP_OK) {
        ESP_LOGW(TAG, "local prompt begin failed: epoch=%u", (unsigned)epoch);
        return false;
    }
    for (size_t index = 0U; index < segment_count; ++index)
    {
        const uint8_t *segment = segments[index].start;
        const size_t segment_bytes = (size_t)(segments[index].end - segment);
        size_t offset = 0U;
        while (offset < segment_bytes)
        {
            const size_t chunk = (segment_bytes - offset) > 2048U ? 2048U : (segment_bytes - offset);
            if (audio_playback_service_enqueue_pcm(AUDIO_PLAYOUT_SOURCE_LOCAL_PROMPT, epoch,
                                                  segment + offset, chunk) != ESP_OK)
            {
                ESP_LOGW(TAG, "local prompt enqueue failed: epoch=%u segment=%u offset=%u",
                         (unsigned)epoch, (unsigned)index, (unsigned)offset);
                audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_LOCAL_PROMPT, epoch);
                return false;
            }
            offset += chunk;
        }
    }
    (void)audio_playback_service_finish(AUDIO_PLAYOUT_SOURCE_LOCAL_PROMPT, epoch);
    if (epoch_out != NULL) *epoch_out = epoch;
    s_local_prompt_active = true;
    s_local_prompt_kind = kind;
    const int64_t deadline_us =
        kind == LOCAL_PROMPT_INTENT ? XIAOZHI_LOCAL_PROMPT_DEADLINE_US
                                    : XIAOZHI_LOCAL_SESSION_PROMPT_DEADLINE_US;
    const int64_t queued_us = esp_timer_get_time();
    s_local_prompt_deadline_us = queued_us + deadline_us;
    /* 排队时播放可能已经开始了几十毫秒，所以这个估值偏晚。偏晚意味着更容易走
       “让它说完”——正是我们想要的方向。 */
    s_local_prompt_end_us =
        queued_us + (int64_t)(total_bytes / XIAOZHI_LOCAL_PROMPT_BYTES_PER_MS) * 1000LL;
    ESP_LOGI(TAG, "local prompt queued: kind=%d epoch=%u segments=%u bytes=%u", (int)kind,
             (unsigned)epoch, (unsigned)segment_count, (unsigned)total_bytes);
    return true;
}

/* Single-asset wrapper: the intent, exit, timeout and fallback prompts are
 * still one embedded file each. */
static bool play_local_prompt_pcm(local_prompt_kind_t kind, const uint8_t *start,
                                  const uint8_t *end, uint32_t *epoch_out)
{
    const local_pcm_segment_t single = {
        .start = start,
        .end = end,
    };
    return play_local_prompt_segments(kind, &single, 1U, epoch_out);
}

/* Starts the cloud TTS reply that was held behind a still-playing local prompt.
 * This is the response-turn subset of the tts start handling in process_text;
 * it must only be called after the prompt FINISHED so begin_tts_playback does
 * not truncate it. */
static void resume_deferred_cloud_tts(void)
{
    s_discard_tts_stream = false;
    s_playing_turn_id = s_pending_cloud_tts_turn_id;
    s_waiting_turn_id = 0U;
    s_greeting_turn_id = 0U;
    s_boot_greeting_id = 0U;
    s_playing_boot_greeting = false;
    s_playing_wake_greeting = false;
    s_playing_exit_prompt = false;
    s_playing_timeout_prompt = false;
    s_playing_fallback_prompt = false;
    s_exit_prompt_pending = false;
    s_timeout_prompt_pending = false;
    s_fallback_prompt_pending = false;
    if (xiaozhi_audio_pipeline_begin_tts_playback(&s_tts_playback_epoch) != ESP_OK) {
        s_discard_tts_stream = true;
        s_playing_turn_id = 0U;
        ESP_LOGW(TAG, "deferred cloud TTS rejected: shared PCM playout unavailable");
        s_state = XIAOZHI_VOICE_WAIT_WAKE;
        publish_ui_state(VOICE_UI_ERROR);
        schedule_ui_idle_after(XIAOZHI_UI_ERROR_HOLD_US);
        return;
    }
    s_state = XIAOZHI_VOICE_TTS_BUFFERING;
    s_followup_deadline_us = 0;
    s_response_deadline_us = 0;
    s_boot_greeting_tts_deadline_us = 0;
    s_tts_start_us = esp_timer_get_time();
    s_tts_last_packet_us = 0;
    s_tts_stall_deadline_us = 0;
    s_tts_first_pcm_deadline_us = s_tts_start_us + XIAOZHI_TTS_FIRST_PCM_TIMEOUT_US;
    s_tts_first_packet_seen = false;
    s_tts_first_pcm_seen = false;
    ESP_LOGI(TAG, "deferred cloud TTS started after local prompt: turn=%u",
             (unsigned)s_playing_turn_id);
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

/* A deferred media start can outlive the acknowledgement that raised it: the
 * cloud may call the tool after its TTS already drained, or the turn may have
 * moved on before the request arrived.  This is the backstop run from the idle
 * 1 s tick so such a request is never silently dropped. */
static void consume_deferred_media_starts(void)
{
    /* While the cloud still owns the speaker, starting either one here would be
     * cut off by (or would fight) the TTS that is about to play, so the turn
     * handlers keep that job. */
    const bool cloud_owns_speaker =
        s_state == XIAOZHI_VOICE_WAIT_RESPONSE ||
        s_state == XIAOZHI_VOICE_TTS_BUFFERING ||
        s_state == XIAOZHI_VOICE_SPEAKING ||
        s_state == XIAOZHI_VOICE_TTS_DRAINING ||
        s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_TTS ||
        s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY ||
        s_state == XIAOZHI_VOICE_ERROR_TONE;
    if (cloud_owns_speaker)
        return;
    /* 没有活跃会话才归还扬声器：LISTENING/WAIT_SPEECH 阶段归还等于对着麦克风放
       电台，立刻又会被自己的声音触发一轮上行（AEC 关闭，这是报错的根因）。 */
    if (!s_session_active)
        (void)radio_stream_service_regain();
    if (s_pending_countdown_start || mcp_service_pending_countdown())
    {
        s_pending_countdown_start = false;
        mcp_service_clear_pending_countdown();
        const timer_service_cmd_result_t result = timer_service_start_pending_countdowns();
        ESP_LOGI(TAG, "deferred countdown finalized by the idle tick: result=%d", (int)result);
    }
    if (s_pending_radio_play || mcp_service_pending_radio())
    {
        /* 必须在 clear 之前取：它指向的正是 clear 要清的那块缓冲。 */
        char wanted_station[24];
        strncpy(wanted_station, mcp_service_pending_radio_station(), sizeof(wanted_station) - 1U);
        wanted_station[sizeof(wanted_station) - 1U] = '\0';
        s_pending_radio_play = false;
        mcp_service_clear_pending_radio();
        /* 电台是这一轮才要求的，收场时不该为它留听音窗口。 */
        s_radio_on_before_turn = false;
        ESP_LOGI(TAG, "deferred radio start released by the idle tick");
        esp_err_t radio_err = radio_stream_service_play_default();
        if (wanted_station[0] != '\0')
        {
            radio_err = radio_stream_service_play_station_by_name(wanted_station);
            if (radio_err == ESP_ERR_NOT_FOUND)
            {
                ESP_LOGW(TAG, "cloud named an unknown station; falling back to the default");
                radio_err = radio_stream_service_play_default();
            }
        }
        if (radio_err != ESP_OK)
        {
            ESP_LOGW(TAG, "radio start failed from the idle tick: %s", esp_err_to_name(radio_err));
        }
    }
}

static void close_channel(bool refresh_credentials)
{
    /* Consumed up front: a duplicate close must not leave the flag set and
     * swallow the error face of a later, real fault. */
    const bool clean_exit = s_session_exit_clean;
    s_session_exit_clean = false;
    /* 会话收尾：下一次唤醒重新拥有一次“没听清”重开。 */
    s_followup_retry_used = false;
    if (s_client_cleanup_in_progress)
    {
        ESP_LOGW(TAG, "WSS cleanup already owned; duplicate close ignored");
        return;
    }
    s_client_cleanup_in_progress = true;
    const bool had_active_session = s_session_active;
    /* A deferred "play radio" must survive this teardown: the stream is
     * independent of the voice session, so it is released at the end of the
     * function instead of being dropped with the rest of the turn state. */
    const bool release_radio_after_close =
        s_pending_radio_play || mcp_service_pending_radio();
    /* 用户拍板：唤醒时电台就已经在播的那段对话，一旦收场就把电台一并结束，
       下次要听重新说"打开电台"。本轮自己打开的不算——启动电台时会把
       s_radio_on_before_turn 清掉，那是用户当场要听的结果，收场只归还扬声器。 */
    const bool radio_played_through_turn = s_radio_on_before_turn;
    if (s_pending_countdown_start || mcp_service_pending_countdown())
    {
        s_pending_countdown_start = false;
        mcp_service_clear_pending_countdown();
        const timer_service_cmd_result_t result = timer_service_start_pending_countdowns();
        ESP_LOGI(TAG, "deferred countdown finalized on voice chain exit: result=%d",
                 (int)result);
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
    /* A closing session must also silence a local confirmation prompt (e.g. the
     * watchdog or a timer ring closed the turn before it finished). */
    if (s_local_prompt_epoch != 0U)
    {
        audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_LOCAL_PROMPT, s_local_prompt_epoch);
    }
    s_local_prompt_active = false;
    s_local_prompt_deadline_us = 0;
    s_local_prompt_end_us = 0;
    s_local_prompt_kind = LOCAL_PROMPT_NONE;
    s_pending_cloud_tts_after_prompt = false;
    s_pending_cloud_tts_turn_id = 0U;
    s_pending_radio_play = false;
    mcp_service_clear_pending_radio();
    /* 本地电台命令的屏蔽只服务本轮：下一轮从头判断云端该不该碰电台。 */
    mcp_service_set_radio_tools_blocked(false);
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
    s_tts_first_pcm_deadline_us = 0;
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
    else if (had_active_session && !clean_exit)
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
        /* 电台（含让位/暂停）在播期间保持热连接。release_radio_after_close 说明
           电台是在本次关闭之后才启动的，所以它也要算进来。 */
        const bool prewarm_radio_on =
            release_radio_after_close || radio_stream_service_is_requested();
        const int64_t prewarm_delay_us =
            prewarm_radio_on ? XIAOZHI_PREWARM_RADIO_DELAY_US : XIAOZHI_PREWARM_RETRY_US;
        s_prewarm_retry_us = esp_timer_get_time() + prewarm_delay_us;
        s_prewarm_retry_delay_us = 0; /* fresh schedule: reset the backoff ramp */
        ESP_LOGI(TAG, "WSS idle prewarm scheduled: delay_ms=%lld radio=%d",
                 (long long)(prewarm_delay_us / 1000LL), (int)prewarm_radio_on);
    }
    /* Released last: every playback abort above would have killed a stream that
     * had been started earlier in this teardown. */
    if (release_radio_after_close)
    {
        ESP_LOGI(TAG, "deferred radio start released after the voice chain closed");
        if (radio_stream_service_play_default() != ESP_OK)
        {
            ESP_LOGW(TAG, "radio start failed after the voice chain closed");
        }
        (void)radio_stream_service_regain();
    }
    else if (radio_played_through_turn)
    {
        ESP_LOGI(TAG, "voice session over; the radio that played through it stops here");
        (void)radio_stream_service_stop();
    }
    else
    {
        /* 会话收尾必须归还扬声器：错误音、超时关闭、唤醒后没说话就退出等路径都会
           走到这里，让位不解除的话电台就一直是静音的。 */
        (void)radio_stream_service_regain();
    }
    s_client_cleanup_in_progress = false;
    /* 这段对话结束，下一次唤醒重新判定"电台是不是已经在放"。 */
    s_radio_on_before_turn = false;
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
    /* The greeting is a local prompt now; drop its bookkeeping so a late
     * FINISHED event cannot re-enter the reply window after the failure. */
    if (s_local_prompt_kind == LOCAL_PROMPT_BOOT_GREETING)
    {
        s_local_prompt_active = false;
        s_local_prompt_deadline_us = 0;
        s_local_prompt_end_us = 0;
        s_local_prompt_kind = LOCAL_PROMPT_NONE;
        if (s_local_prompt_epoch != 0U)
            audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_LOCAL_PROMPT, s_local_prompt_epoch);
    }
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

/* Declares the _binary_* start/end pair of one embedded asset.  The naming
 * rule (<symbol>_16k_mono_s16le.pcm) is shared with generate_prompt_tones.py. */
#define DECLARE_EMBEDDED_PCM(symbol)                                             \
    extern const uint8_t symbol##_pcm_start[] asm(                               \
        "_binary_" #symbol "_16k_mono_s16le_pcm_start");                         \
    extern const uint8_t symbol##_pcm_end[] asm(                                 \
        "_binary_" #symbol "_16k_mono_s16le_pcm_end")

DECLARE_EMBEDDED_PCM(prompt_greet_forenoon);
DECLARE_EMBEDDED_PCM(prompt_greet_noon);
DECLARE_EMBEDDED_PCM(prompt_greet_afternoon);
DECLARE_EMBEDDED_PCM(prompt_greet_evening);
DECLARE_EMBEDDED_PCM(prompt_greet_night);
DECLARE_EMBEDDED_PCM(prompt_greet_generic);
DECLARE_EMBEDDED_PCM(prompt_greet_closing);

DECLARE_EMBEDDED_PCM(prompt_hour_00);
DECLARE_EMBEDDED_PCM(prompt_hour_01);
DECLARE_EMBEDDED_PCM(prompt_hour_02);
DECLARE_EMBEDDED_PCM(prompt_hour_03);
DECLARE_EMBEDDED_PCM(prompt_hour_04);
DECLARE_EMBEDDED_PCM(prompt_hour_05);
DECLARE_EMBEDDED_PCM(prompt_hour_06);
DECLARE_EMBEDDED_PCM(prompt_hour_07);
DECLARE_EMBEDDED_PCM(prompt_hour_08);
DECLARE_EMBEDDED_PCM(prompt_hour_09);
DECLARE_EMBEDDED_PCM(prompt_hour_10);
DECLARE_EMBEDDED_PCM(prompt_hour_11);
DECLARE_EMBEDDED_PCM(prompt_hour_12);
DECLARE_EMBEDDED_PCM(prompt_hour_13);
DECLARE_EMBEDDED_PCM(prompt_hour_14);
DECLARE_EMBEDDED_PCM(prompt_hour_15);
DECLARE_EMBEDDED_PCM(prompt_hour_16);
DECLARE_EMBEDDED_PCM(prompt_hour_17);
DECLARE_EMBEDDED_PCM(prompt_hour_18);
DECLARE_EMBEDDED_PCM(prompt_hour_19);
DECLARE_EMBEDDED_PCM(prompt_hour_20);
DECLARE_EMBEDDED_PCM(prompt_hour_21);
DECLARE_EMBEDDED_PCM(prompt_hour_22);
DECLARE_EMBEDDED_PCM(prompt_hour_23);

DECLARE_EMBEDDED_PCM(prompt_wx_clear);
DECLARE_EMBEDDED_PCM(prompt_wx_cloudy);
DECLARE_EMBEDDED_PCM(prompt_wx_fog);
DECLARE_EMBEDDED_PCM(prompt_wx_rain);
DECLARE_EMBEDDED_PCM(prompt_wx_snow);
DECLARE_EMBEDDED_PCM(prompt_wx_thunder);
DECLARE_EMBEDDED_PCM(prompt_wx_unknown);

/* One embedded asset plus the bucket name used in COM6 diagnostics. */
typedef struct
{
    const uint8_t *start;
    const uint8_t *end;
    const char *name;
} boot_greeting_asset_t;

/* Period greeting.  The value is the index into s_boot_period_pcm. */
typedef enum
{
    BOOT_PERIOD_FORENOON = 0, /* 05:00 - 11:59 */
    BOOT_PERIOD_NOON,         /* 12:00 - 12:59 */
    BOOT_PERIOD_AFTERNOON,    /* 13:00 - 17:59 */
    BOOT_PERIOD_EVENING,      /* 18:00 - 22:59 */
    BOOT_PERIOD_NIGHT,        /* 23:00 - 04:59 */
    BOOT_PERIOD_GENERIC,      /* clock not refreshed yet */
    BOOT_PERIOD_COUNT,
} boot_period_t;

/* Weather copy.  The value is the index into s_boot_weather_pcm. */
typedef enum
{
    BOOT_WEATHER_CLEAR = 0,
    BOOT_WEATHER_CLOUDY,
    BOOT_WEATHER_FOG,
    BOOT_WEATHER_RAIN,
    BOOT_WEATHER_SNOW,
    BOOT_WEATHER_THUNDER,
    BOOT_WEATHER_UNKNOWN,
    BOOT_WEATHER_COUNT,
} boot_weather_t;

static const boot_greeting_asset_t s_boot_period_pcm[BOOT_PERIOD_COUNT] = {
    {prompt_greet_forenoon_pcm_start, prompt_greet_forenoon_pcm_end, "forenoon"},
    {prompt_greet_noon_pcm_start, prompt_greet_noon_pcm_end, "noon"},
    {prompt_greet_afternoon_pcm_start, prompt_greet_afternoon_pcm_end, "afternoon"},
    {prompt_greet_evening_pcm_start, prompt_greet_evening_pcm_end, "evening"},
    {prompt_greet_night_pcm_start, prompt_greet_night_pcm_end, "night"},
    {prompt_greet_generic_pcm_start, prompt_greet_generic_pcm_end, "generic"},
};

/* Indexed by struct tm::tm_hour, so the live hour is a direct table index. */
static const boot_greeting_asset_t s_boot_hour_pcm[24] = {
    {prompt_hour_00_pcm_start, prompt_hour_00_pcm_end, "hour_00"},
    {prompt_hour_01_pcm_start, prompt_hour_01_pcm_end, "hour_01"},
    {prompt_hour_02_pcm_start, prompt_hour_02_pcm_end, "hour_02"},
    {prompt_hour_03_pcm_start, prompt_hour_03_pcm_end, "hour_03"},
    {prompt_hour_04_pcm_start, prompt_hour_04_pcm_end, "hour_04"},
    {prompt_hour_05_pcm_start, prompt_hour_05_pcm_end, "hour_05"},
    {prompt_hour_06_pcm_start, prompt_hour_06_pcm_end, "hour_06"},
    {prompt_hour_07_pcm_start, prompt_hour_07_pcm_end, "hour_07"},
    {prompt_hour_08_pcm_start, prompt_hour_08_pcm_end, "hour_08"},
    {prompt_hour_09_pcm_start, prompt_hour_09_pcm_end, "hour_09"},
    {prompt_hour_10_pcm_start, prompt_hour_10_pcm_end, "hour_10"},
    {prompt_hour_11_pcm_start, prompt_hour_11_pcm_end, "hour_11"},
    {prompt_hour_12_pcm_start, prompt_hour_12_pcm_end, "hour_12"},
    {prompt_hour_13_pcm_start, prompt_hour_13_pcm_end, "hour_13"},
    {prompt_hour_14_pcm_start, prompt_hour_14_pcm_end, "hour_14"},
    {prompt_hour_15_pcm_start, prompt_hour_15_pcm_end, "hour_15"},
    {prompt_hour_16_pcm_start, prompt_hour_16_pcm_end, "hour_16"},
    {prompt_hour_17_pcm_start, prompt_hour_17_pcm_end, "hour_17"},
    {prompt_hour_18_pcm_start, prompt_hour_18_pcm_end, "hour_18"},
    {prompt_hour_19_pcm_start, prompt_hour_19_pcm_end, "hour_19"},
    {prompt_hour_20_pcm_start, prompt_hour_20_pcm_end, "hour_20"},
    {prompt_hour_21_pcm_start, prompt_hour_21_pcm_end, "hour_21"},
    {prompt_hour_22_pcm_start, prompt_hour_22_pcm_end, "hour_22"},
    {prompt_hour_23_pcm_start, prompt_hour_23_pcm_end, "hour_23"},
};

static const boot_greeting_asset_t s_boot_weather_pcm[BOOT_WEATHER_COUNT] = {
    {prompt_wx_clear_pcm_start, prompt_wx_clear_pcm_end, "clear"},
    {prompt_wx_cloudy_pcm_start, prompt_wx_cloudy_pcm_end, "cloudy"},
    {prompt_wx_fog_pcm_start, prompt_wx_fog_pcm_end, "fog"},
    {prompt_wx_rain_pcm_start, prompt_wx_rain_pcm_end, "rain"},
    {prompt_wx_snow_pcm_start, prompt_wx_snow_pcm_end, "snow"},
    {prompt_wx_thunder_pcm_start, prompt_wx_thunder_pcm_end, "thunder"},
    {prompt_wx_unknown_pcm_start, prompt_wx_unknown_pcm_end, "unknown"},
};

/* Fourth segment - the fixed help offer.  Unlike the three tables above it
 * offers no choices: it is appended verbatim after the weather copy so the
 * greeting ends on an invitation to talk instead of a statement about the
 * weather. */
static const boot_greeting_asset_t s_boot_closing_pcm = {
    prompt_greet_closing_pcm_start, prompt_greet_closing_pcm_end, "closing"};

static boot_period_t boot_period_from_hour(int hour)
{
    if (hour >= 5 && hour <= 11) return BOOT_PERIOD_FORENOON;
    if (hour == 12) return BOOT_PERIOD_NOON;
    if (hour >= 13 && hour <= 17) return BOOT_PERIOD_AFTERNOON;
    if (hour >= 18 && hour <= 22) return BOOT_PERIOD_EVENING;
    return BOOT_PERIOD_NIGHT;
}

/* Maps weather_service.c's weather_text() onto a spoken weather line.  Note
 * that weather_text() returns "未知" when a WMO code has no mapping: that must
 * not fall through to the clear-weather copy. */
static boot_weather_t boot_weather_from_text(const char *weather)
{
    if (weather == NULL || weather[0] == '\0') return BOOT_WEATHER_UNKNOWN;
    if (strstr(weather, "雪") != NULL || strstr(weather, "冰") != NULL) return BOOT_WEATHER_SNOW;
    if (strstr(weather, "雷") != NULL) return BOOT_WEATHER_THUNDER;
    if (strstr(weather, "雨") != NULL) return BOOT_WEATHER_RAIN;
    if (strstr(weather, "雾") != NULL) return BOOT_WEATHER_FOG;
    if (strstr(weather, "云") != NULL || strstr(weather, "阴") != NULL) return BOOT_WEATHER_CLOUDY;
    if (strstr(weather, "晴") != NULL) return BOOT_WEATHER_CLEAR;
    return BOOT_WEATHER_UNKNOWN;
}

/* Picks the embedded greeting segments for this boot.
 *
 * The official xiaozhi.me server rejects the old [device_call] direct-TTS
 * prefix, so the greeting is assembled from pre-rendered PCM instead of a
 * cloud request.  Four segments are concatenated:
 *
 *   [period greeting] + [spoken clock] + [weather copy] + [help offer]
 *
 * Every segment carries its own time modifier ("现在是上午7点" vs
 * "现在是晚上7点"), which is what the old shared-clip design could not say.
 * It costs 6 + 24 + 7 + 1 = 38 short assets instead of hundreds of whole
 * sentences.  When the clock has not been refreshed yet the spoken clock is
 * dropped and the generic period greeting is used instead. */
static size_t select_boot_greeting_segments(local_pcm_segment_t *segments, size_t capacity)
{
    if (segments == NULL || capacity < 4U)
        return 0U;

    lumidock_snapshot_t snapshot = {0};
    mid_snapshot_get(&snapshot);
    const time_weather_snapshot_t *tw = &snapshot.time_weather;
    const char *weather = tw->valid ? tw->weather_text : "";
    const boot_weather_t weather_pick = boot_weather_from_text(weather);

    /* local_time is only meaningful once time_service_poll() has refreshed it,
     * which needs a valid RTC or a finished SNTP sync; tm_year stays 0 until
     * then, and reading tm_hour would invent a bogus hour. */
    const bool clock_ready = tw->valid && tw->local_time.tm_year > 0 &&
                             tw->local_time.tm_hour >= 0 && tw->local_time.tm_hour <= 23;
    const int hour = clock_ready ? tw->local_time.tm_hour : 0;
    const boot_period_t period =
        clock_ready ? boot_period_from_hour(hour) : BOOT_PERIOD_GENERIC;

    size_t count = 0U;
    segments[count].start = s_boot_period_pcm[period].start;
    segments[count].end = s_boot_period_pcm[period].end;
    ++count;
    if (clock_ready)
    {
        segments[count].start = s_boot_hour_pcm[hour].start;
        segments[count].end = s_boot_hour_pcm[hour].end;
        ++count;
    }
    segments[count].start = s_boot_weather_pcm[weather_pick].start;
    segments[count].end = s_boot_weather_pcm[weather_pick].end;
    ++count;
    /* The help offer closes every greeting, including the degraded one without
     * a spoken clock, so the segment count is 4 or 3 and never fewer. */
    segments[count].start = s_boot_closing_pcm.start;
    segments[count].end = s_boot_closing_pcm.end;
    ++count;

    ESP_LOGI(TAG, "boot greeting PCM bucket=%s weather=%s hour=%s closing=%s segments=%u",
             s_boot_period_pcm[period].name, weather,
             clock_ready ? s_boot_hour_pcm[hour].name : "none",
             s_boot_closing_pcm.name, (unsigned)count);
    return count;
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
    /* 方案 A 试验：让云端现场生成问候，不再播本地预置 PCM。
     *
     * 原实现是选几个本地片段（时段问候 + 报时 + 天气 + 收尾句）拼着播。
     * 片段是预先录好的，时间和城市只能分档到“凌晨/上午/下雨/晴”，说不出
     * “8 点 15 分”“北京今天小雨转阴”这种具体内容，所以改成在开机连上云
     * 端之后主动发一条 detect，把 text 写成一句问题，赌官方服务器会把这段
     * 文本当成用户说的话交给 LLM。
     *
     * 成败就看云端回什么：
     *   回“早上好，现在是 8 点 15 分，北京今天晴……” → 方案 A 成立，
     *     本地问候片段（含 select_boot_greeting_segments、prompt_greet_*、
     *     prompt_hour_00~23）整条链路都可以删，能省下约 1.16 MB 资源
     *   回“我在”之类的固定唤醒应答 → 不成立，text 只是唤醒词的标注，
     *     得走方案 B（用上行音频冒充用户提问）
     *
     * 这段文本会被服务器当作“用户说的话”，所以必须是用户口吻的问题，
     * 不能写成“你好小智”那种唤醒词。 */
    if (!send_text("{\"type\":\"listen\",\"state\":\"detect\","
                   "\"text\":\"现在几点了，今天天气怎么样\"}"))
    {
        finish_boot_greeting_failure("greeting detect send failed");
        return;
    }
    ++s_turn_id;
    if (s_turn_id == 0U)
        ++s_turn_id;
    s_boot_greeting_id = s_turn_id;
    /* 必须同时置 greeting_turn_id：tts start 处理器只接受 response_turn
     * 和 greeting_turn，boot_greeting/exit/timeout/fallback 四个 turn 一律
     * 丢弃。漏了这行，云端的问候会被当成陈旧流丢掉，设备一声不响。 */
    s_greeting_turn_id = s_turn_id;
    s_waiting_turn_id = 0U;
    s_playing_turn_id = 0U;
    s_session_active = true;
    s_state = XIAOZHI_VOICE_WAIT_SPEECH;
    s_boot_greeting_tts_deadline_us = 0;
    s_boot_greeting_reply_deadline_us = 0;
    ESP_LOGI(TAG, "boot greeting sent to the cloud as a question: id=%u",
             (unsigned)s_boot_greeting_id);
    publish_ui_state(VOICE_UI_LISTENING);
    start_listen(true);
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
    /* A follow-up has to clear a higher bar than the boot greeting: the user
     * has just been talking to the device, so real speech is close-mic loud,
     * while a room burst must never consume the bounded follow-up window. */
    const uint32_t bypass_peak = followup_reply ? XIAOZHI_FOLLOWUP_DIRECT_REPLY_MIN_PEAK
                                                : XIAOZHI_DIRECT_REPLY_BYPASS_PEAK;
    if (recent_peak < bypass_peak)
    {
        s_post_playback_silence_deadline_us = esp_timer_get_time() +
                                              XIAOZHI_POST_PLAYBACK_SILENCE_GUARD_US;
        ESP_LOGI(TAG, "post-playback VAD start retained silence guard: peak=%u threshold=%u deadline_extended_ms=%u",
                 (unsigned)recent_peak, (unsigned)bypass_peak,
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
    ESP_LOGI(TAG, "wake greeting PCM drained; fresh VAD baseline, follow-up window=%lldms",
             (long long)(XIAOZHI_FOLLOWUP_WINDOW_US / 1000LL));
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
        s_session_exit_clean = true;
        close_channel(false);
        return;
    }
    if (timeout_prompt_finished)
    {
        ESP_LOGI(TAG, "timeout prompt PCM drained; follow-up window=%lldms",
                 (long long)(XIAOZHI_FOLLOWUP_WINDOW_US / 1000LL));
    }
    if (fallback_prompt_finished)
    {
        ESP_LOGI(TAG, "fallback prompt PCM drained; follow-up window=%lldms",
                 (long long)(XIAOZHI_FOLLOWUP_WINDOW_US / 1000LL));
    }
    s_vad_speech = false;
    arm_post_playback_silence_gate();
    if (boot_greeting_finished)
        s_boot_greeting_direct_window_active = true;
    /* 电台必须排在开机问候窗口前面：问候窗口的标记 (s_boot_greeting_direct_window_active)
       要一直挂到下一次 close_channel 才清，压在后面会把"开电台"那一轮截走。 */
    if (radio_stream_service_is_requested())
    {
        if (s_radio_on_before_turn)
        {
            /* 用户是打断正在播的电台进来的：先把听音窗口留满，再归还扬声器。窗口期间
               电台保持让位——也就是"停拉流、关解码"的哑状态——麦克风收到的是安静的
               房间，所以不存在旧版那种"对着麦克风放电台"的自激上行。窗口内用户直接
               开口就能接上下一轮；到期由 tick 直接归还扬声器恢复直播。 */
            s_state = XIAOZHI_VOICE_WAIT_FOLLOWUP;
            s_followup_deadline_us = esp_timer_get_time() + XIAOZHI_FOLLOWUP_WINDOW_US;
            s_ui_idle_deadline_us = 0;
            ESP_LOGI(TAG, "电台让位保持；先留 follow-up window=%lldms 再归还扬声器",
                     (long long)(XIAOZHI_FOLLOWUP_WINDOW_US / 1000LL));
        }
        else
        {
            /* 电台请求是这一轮才下来的（用户说"开电台"）：他要的就是电台出声，
               不能压着听音窗口，立即归还扬声器并收场。 */
            s_session_exit_clean = true;
            ESP_LOGI(TAG, "电台在本轮启动；立即归还扬声器并结束本轮");
            close_channel(false);
            return;
        }
    }
    else if (boot_greeting_window_active)
    {
        s_boot_greeting_reply_deadline_us = esp_timer_get_time() +
                                            XIAOZHI_BOOT_GREETING_REPLY_WINDOW_US;
        s_state = XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY;
        s_ui_idle_deadline_us = 0;
        if (boot_greeting_finished)
        {
            ESP_LOGI(TAG, "boot greeting PCM drained; direct reply window=%lldms",
                     (long long)(XIAOZHI_BOOT_GREETING_REPLY_WINDOW_US / 1000LL));
        }
        else
        {
            ESP_LOGI(TAG, "boot greeting response PCM drained; direct reply window re-armed=%lldms",
                     (long long)(XIAOZHI_BOOT_GREETING_REPLY_WINDOW_US / 1000LL));
        }
    }
    else
    {
        s_state = XIAOZHI_VOICE_WAIT_FOLLOWUP;
        s_followup_deadline_us = esp_timer_get_time() + XIAOZHI_FOLLOWUP_WINDOW_US;
        s_ui_idle_deadline_us = 0;
        ESP_LOGI(TAG, "cloud TTS PCM drained; fresh VAD baseline, follow-up window=%lldms",
                 (long long)(XIAOZHI_FOLLOWUP_WINDOW_US / 1000LL));
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

/* Dispatches the shared local-prompt playout once it has drained (or once its
 * watchdog fired).  Intent prompts only release a queued cloud reply, while the
 * session prompts each resume their own part of the state machine - exactly
 * what the cloud [device_call] TTS path used to do when its stream stopped. */
static void finish_local_prompt_playback(void)
{
    const local_prompt_kind_t kind = s_local_prompt_kind;
    s_local_prompt_kind = LOCAL_PROMPT_NONE;
    switch (kind)
    {
    case LOCAL_PROMPT_BOOT_GREETING:
        s_playing_boot_greeting = true;
        enter_followup_after_playback();
        break;
    case LOCAL_PROMPT_EXIT:
        s_playing_exit_prompt = true;
        enter_followup_after_playback();
        break;
    case LOCAL_PROMPT_TIMEOUT:
        s_playing_timeout_prompt = true;
        enter_followup_after_playback();
        break;
    case LOCAL_PROMPT_FALLBACK:
        s_playing_fallback_prompt = true;
        enter_followup_after_playback();
        break;
    case LOCAL_PROMPT_INTENT:
    case LOCAL_PROMPT_NONE:
    default:
        if (s_pending_cloud_tts_after_prompt)
        {
            s_pending_cloud_tts_after_prompt = false;
            resume_deferred_cloud_tts();
        }
        break;
    }
}

/* 云端回复的音频真的到了：本地“正在执行”提示音到此为止，立刻让位。
 *
 * 这一段原来是把包丢掉、等提示音放完再开 TTS。实测云端回得比提示音快得多：
 * 日志 67530 提示音开始，67560 第一个音频包就到了，直到 68380 提示音放完为止
 * 一共丢掉约 40 个包（约 2.4 s）；恢复播放时只剩 10 帧，“电台已经在播放中啦～”
 * 只剩下尾巴，前两个字整段没出声，紧接着 70780 就 underrun，听起来就是卡卡的。
 * 让位早一点只是把提示音截断，云端的字一个都不会丢。
 *
 * 现在再补一层优雅收尾：云端音频到达时若提示音只剩不到 XIAOZHI_LOCAL_PROMPT_GRACE_US，
 * 就让它把最后两个字说完再让位。见下面 release_deferred_cloud_tts_on_audio()。
 *
 * 只从 VOICE_EVENT_WSS_BINARY 开头一行调用：逻辑放这里是为了不往那个分支里塞
 * 新的大括号。test_voice_single_turn_tts_contract.ps1 用 IndexOf('    }') 切那段
 * 分支，插进去的代码只要带大括号就会把切点提到守卫之前，断言直接挂掉。 */
static void release_deferred_cloud_tts_on_audio(uint32_t connection_seq)
{
    if (!s_pending_cloud_tts_after_prompt || connection_seq != s_wss_connection_seq)
    {
        return;
    }
    /* 云端音频到了。剩不到 GRACE 就让它说完：不 abort、也不清
       s_pending_cloud_tts_after_prompt——不清是有意的，VOICE_EVENT_LOCAL_PROMPT_FINISHED
       的自然收尾会看到它还在，替我们 resume 云端 TTS。 */
    const int64_t remaining_us = s_local_prompt_end_us - esp_timer_get_time();
    if (s_local_prompt_active && remaining_us > 0 &&
        remaining_us <= XIAOZHI_LOCAL_PROMPT_GRACE_US)
    {
        ESP_LOGI(TAG, "cloud reply audio arrived; local prompt finishes first: turn=%u remaining_ms=%lld",
                 (unsigned)s_pending_cloud_tts_turn_id, (long long)(remaining_us / 1000LL));
        return;
    }
    ESP_LOGI(TAG, "cloud reply audio arrived; local prompt yields: turn=%u",
             (unsigned)s_pending_cloud_tts_turn_id);
    if (s_local_prompt_epoch != 0U)
    {
        audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_LOCAL_PROMPT, s_local_prompt_epoch);
    }
    s_local_prompt_active = false;
    s_local_prompt_deadline_us = 0;
    s_local_prompt_end_us = 0;
    s_local_prompt_kind = LOCAL_PROMPT_NONE;
    s_pending_cloud_tts_after_prompt = false;
    resume_deferred_cloud_tts();
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
        .pingpong_timeout_sec = 20,
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
    /* Discovery belongs to one session: a fresh connection has to prove again
     * that the cloud speaks MCP before the local volume shortcut steps aside. */
    mcp_service_reset();
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

/* 电台让位是异步的：yield() 只是置位，拉流任务要等它自己那一轮循环退出会话，才会
   关掉 HTTP/TLS 把内部 RAM 还回来。WSS 的 websocket task 要 4096 字节连续内部
   RAM，实测电台拉流时只剩 4631 字节 / 最大连续块 1920，所以面对一个已经拉流成功
   的电台，立刻建连是必败的。这里等它停稳再走，最多 XIAOZHI_RADIO_PARK_WAIT_US。 */
static void wait_for_radio_park(void)
{
    if (!radio_stream_service_is_requested())
        return;
    const int64_t deadline_us = esp_timer_get_time() + XIAOZHI_RADIO_PARK_WAIT_US;
    while (radio_stream_service_is_active() && esp_timer_get_time() < deadline_us)
        vTaskDelay(pdMS_TO_TICKS(10));
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
    /* 走到这里说明没有可复用的热连接，必须现建。唤醒处理已经在前面调过 yield()，
       但那是异步的；等电台真的退出会话、把内部 RAM 还回来再建，否则 4096 字节
       连续块要不到，白白吃一个"连接失败"。（只在前台路径等：预连接路径里电台还
       在正常拉流，不该为了它把 tick 拖住。） */
    wait_for_radio_park();
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
            /* 每句新话重新判定：上一句禁掉的云端电台工具不能连坐到这一句。
               follow-up 里接着说“打开一下电台”时，本地没覆盖这种说法，还得
               让云端的 self.radio.play 有机会执行。 */
            mcp_service_set_radio_tools_blocked(false);
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
                /* The cloud's response is the standalone spoken answer.  The
                 * local prompt below is only a transient acknowledgment; a
                 * radio command still starts after that response drains. */
                if (result == VOICE_IOT_COMMAND_RADIO_PLAY)
                {
                    s_pending_radio_play = true;
                }
                /* 本地刚动过电台：这一轮云端的电台工具一律让路。日志里"关闭电台"
                   被云端 ASR 截成"电台。"，同一个模型据此调 self.radio.play，
                   把刚停掉的电台重新拉起来。 */
                if (result == VOICE_IOT_COMMAND_RADIO_PLAY ||
                    result == VOICE_IOT_COMMAND_RADIO_HANDLED)
                {
                    mcp_service_set_radio_tools_blocked(true);
                }
                /* Immediate "正在执行" confirmation before the cloud ack. */
                extern const uint8_t prompt_executing_pcm_start[] asm(
                    "_binary_prompt_executing_16k_mono_s16le_pcm_start");
                extern const uint8_t prompt_executing_pcm_end[] asm(
                    "_binary_prompt_executing_16k_mono_s16le_pcm_end");
                (void)play_local_prompt_pcm(LOCAL_PROMPT_INTENT, prompt_executing_pcm_start,
                                            prompt_executing_pcm_end, NULL);
            }
            else
            {
                /* Not a local command: classify query vs chat to pick the prompt. */
                extern const uint8_t prompt_querying_pcm_start[] asm(
                    "_binary_prompt_querying_16k_mono_s16le_pcm_start");
                extern const uint8_t prompt_querying_pcm_end[] asm(
                    "_binary_prompt_querying_16k_mono_s16le_pcm_end");
                extern const uint8_t prompt_thinking_pcm_start[] asm(
                    "_binary_prompt_thinking_16k_mono_s16le_pcm_start");
                extern const uint8_t prompt_thinking_pcm_end[] asm(
                    "_binary_prompt_thinking_16k_mono_s16le_pcm_end");
                if (is_query_intent(text->valuestring))
                {
                    (void)play_local_prompt_pcm(LOCAL_PROMPT_INTENT, prompt_querying_pcm_start,
                                                prompt_querying_pcm_end, NULL);
                }
                else
                {
                    (void)play_local_prompt_pcm(LOCAL_PROMPT_INTENT, prompt_thinking_pcm_start,
                                                prompt_thinking_pcm_end, NULL);
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
            /* The boot greeting, exit, timeout and fallback prompts are all
             * embedded PCM assets now, so the cloud can never answer them with
             * TTS.  Their pending markers survive only for bookkeeping and the
             * one-shot retry budget; accepting a stale cloud TTS here would
             * reset the shared playout and truncate the local prompt that is
             * still speaking.  Only a real response turn and the single
             * registered wake greeting may accept a cloud TTS start. */
            const bool boot_greeting_turn = false;
            const bool exit_turn = false;
            const bool timeout_turn = false;
            const bool fallback_turn = false;
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
            if (s_local_prompt_active && response_turn)
            {
                /* A valid cloud reply arrived while the local confirmation
                 * prompt is still playing: hold it so begin_tts_playback (which
                 * discards the shared playout) cannot truncate the prompt.  The
                 * prompt FINISHED event releases it via resume_deferred_cloud_tts. */
                s_pending_cloud_tts_after_prompt = true;
                s_pending_cloud_tts_turn_id = s_waiting_turn_id;
                s_discard_tts_stream = true;
                ESP_LOGI(TAG, "cloud tts start deferred behind local prompt: turn=%u",
                         (unsigned)s_waiting_turn_id);
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
            s_tts_first_pcm_deadline_us = s_tts_start_us + XIAOZHI_TTS_FIRST_PCM_TIMEOUT_US;
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
    else if (strcmp(type->valuestring, "mcp") == 0)
    {
        /* The cloud can only control the volume once it has discovered the
         * tools we answer, so this branch is what makes "提高音量" work. */
        const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
        const char *reply = mcp_service_handle(payload);
        if (reply != NULL)
        {
            const int written = snprintf(s_mcp_tx_frame, sizeof(s_mcp_tx_frame),
                                         "{\"session_id\":\"%s\",\"type\":\"mcp\",\"payload\":%s}",
                                         s_session_id[0] != '\0' ? s_session_id : "-", reply);
            if (written > 0 && (size_t)written < sizeof(s_mcp_tx_frame))
            {
                if (!send_ws_text_raw(s_mcp_tx_frame))
                {
                    ESP_LOGW(TAG, "MCP reply send failed; the cloud will retry its request");
                }
            }
            else
            {
                ESP_LOGW(TAG, "MCP reply frame dropped: %d bytes needed", written);
            }
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
    /* 抢扬声器之前先让电台退开：让位和停止不是一回事，电台的整体流程要活着。 */
    (void)radio_stream_service_yield();
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
            if (s_pending_countdown_start || mcp_service_pending_countdown())
            {
                s_pending_countdown_start = false;
                mcp_service_clear_pending_countdown();
                const timer_service_cmd_result_t result = timer_service_start_pending_countdowns();
                ESP_LOGI(TAG, "deferred countdown finalized after TTS interruption: result=%d",
                         (int)result);
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
        /* 电台让位必须在开麦之前：唤醒词是本地识别的（不需要上行），所以这一步之后
           麦克风环境才是干净的。让位会把电台的拉流和解码整段停掉，把 core 1 和
           网络缓冲让给这一轮语音，答完由 close_channel 调 regain() 重新接回直播。 */
        /* 这里取 is_requested() 而不是 is_active()：后者在"电台已 regain、还在重连
           尚未出声"时为假，偏偏那也是用户眼里的"电台模式"——用 is_active 会让这种
           时刻唤醒的对话答完立刻把扬声器交回去。is_requested 在电台刚启动(还在
           CONNECTING)时同样为真，覆盖更完整；用户暂停过电台时它为假，走普通窗口。 */
        s_radio_on_before_turn = radio_stream_service_is_requested();
        (void)radio_stream_service_yield();
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
            /* Room noise must not consume the bounded follow-up window: the
             * window has to expire so the session closes on its own.  Only a
             * close-mic peak restarts a cloud turn. */
            if (recent_peak < XIAOZHI_FOLLOWUP_DIRECT_REPLY_MIN_PEAK)
            {
                ESP_LOGI(TAG, "follow-up VAD start gated as ambient noise: peak=%u threshold=%u",
                         (unsigned)recent_peak,
                         (unsigned)XIAOZHI_FOLLOWUP_DIRECT_REPLY_MIN_PEAK);
                break;
            }
            start_followup_listen();
            s_followup_deadline_us = 0;
            /* 用户真的接话了：这一段等待结束，下一段沉默重新拥有一次“没听清”。 */
            s_followup_retry_used = false;
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
            /* The cloud ends a conversation by closing the link right after it
             * finished answering ("终止对话"): the reply already played, so this
             * is a normal exit and must not be reported as a fault.  A drop that
             * still owes the user an answer (speech in flight, response pending,
             * TTS still arriving) keeps the local error tone. */
            const bool turn_delivered = s_state == XIAOZHI_VOICE_WAIT_FOLLOWUP ||
                                        s_state == XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY ||
                                        s_state == XIAOZHI_VOICE_TTS_DRAINING;
            if (turn_delivered)
            {
                ESP_LOGI(TAG, "WSS closed by the server after a delivered turn; ending the session without the fault tone");
                s_session_exit_clean = true;
                close_channel(false);
            }
            else
            {
                /* The user was mid-conversation when the server dropped us:
                 * answer with the local error tone instead of bouncing back to
                 * the main page silently. */
                ESP_LOGW(TAG, "WSS dropped during an active turn; playing local error tone");
                play_error_tone_then_close(false);
            }
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
        release_deferred_cloud_tts_on_audio(event->value);
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
        s_tts_first_pcm_deadline_us = 0;
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
        s_tts_first_pcm_deadline_us = 0;
        /* Either the local shortcut or the cloud MCP tool can have reserved the
         * start; both are released here, once the acknowledgement is audible. */
        if (s_pending_countdown_start || mcp_service_pending_countdown())
        {
            s_pending_countdown_start = false;
            mcp_service_clear_pending_countdown();
            const timer_service_cmd_result_t result = timer_service_start_pending_countdowns();
            ESP_LOGI(TAG, "deferred countdown finalized after cloud TTS PCM drain: result=%d",
                     (int)result);
        }
        if (s_pending_radio_play || mcp_service_pending_radio())
        {
            /* 必须在 clear 之前取：它指向的正是 clear 要清的那块缓冲。 */
            char wanted_station[24];
            strncpy(wanted_station, mcp_service_pending_radio_station(), sizeof(wanted_station) - 1U);
            wanted_station[sizeof(wanted_station) - 1U] = '\0';
            s_pending_radio_play = false;
            mcp_service_clear_pending_radio();
            /* 电台是这一轮才要求的，收场时不该为它留听音窗口。 */
            s_radio_on_before_turn = false;
            ESP_LOGI(TAG, "radio acknowledgement finished; starting radio stream");
            /* 云端点名的频道走包含匹配（ASR 变体的容错在 radio 侧做）。名字对不上就
               退回默认台：宁可放中国之声，也不能让这一轮变得没声音。 */
            esp_err_t radio_err = radio_stream_service_play_default();
            if (wanted_station[0] != '\0')
            {
                radio_err = radio_stream_service_play_station_by_name(wanted_station);
                if (radio_err == ESP_ERR_NOT_FOUND)
                {
                    ESP_LOGW(TAG, "cloud named an unknown station; falling back to the default");
                    radio_err = radio_stream_service_play_default();
                }
            }
            if (radio_err != ESP_OK)
            {
                ESP_LOGW(TAG, "radio start failed after acknowledgement: %s",
                         esp_err_to_name(radio_err));
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
                ESP_LOGW(TAG, "TTS stall watchdog armed; local finish if the stream stays silent for %lld s",
                         (long long)(XIAOZHI_TTS_STALL_TIMEOUT_US / 1000000LL));
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
    case VOICE_EVENT_LOCAL_PROMPT_FINISHED:
        if (event->value != s_local_prompt_epoch || !s_local_prompt_active)
        {
            ESP_LOGI(TAG, "stale local prompt finish ignored: epoch=%u active=%d",
                     (unsigned)event->value, (int)s_local_prompt_active);
            break;
        }
        s_local_prompt_active = false;
        s_local_prompt_deadline_us = 0;
        s_local_prompt_end_us = 0;
        ESP_LOGI(TAG, "local prompt finished: kind=%d", (int)s_local_prompt_kind);
        finish_local_prompt_playback();
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
                    /* 电台在播时立刻保温；其余场景沿用 5 秒，避免刚关掉会话就立刻
                       重连去和用户的追问抢 core 1。 */
                    const bool prewarm_radio_on = radio_stream_service_is_requested();
                    s_prewarm_retry_delay_us = 0; /* fresh schedule: reset the backoff ramp */
                    s_prewarm_retry_us = esp_timer_get_time() +
                                         (prewarm_radio_on ? XIAOZHI_PREWARM_RADIO_DELAY_US
                                                           : XIAOZHI_PREWARM_RETRY_US);
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
        if (s_local_prompt_active && s_local_prompt_deadline_us != 0 &&
            esp_timer_get_time() >= s_local_prompt_deadline_us)
        {
            s_local_prompt_active = false;
            s_local_prompt_deadline_us = 0;
            s_local_prompt_end_us = 0;
            ESP_LOGW(TAG, "local prompt watchdog expired: kind=%d; aborting the shared playout",
                     (int)s_local_prompt_kind);
            audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_LOCAL_PROMPT, s_local_prompt_epoch);
            finish_local_prompt_playback();
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
            /* 电台还等着这次窗口：不请求"拜拜"、不再占用扬声器，直接把直播接回来。
               用户打断电台只是想问一句，收场时再补一句告别反而多余。 */
            if (s_radio_on_before_turn && radio_stream_service_is_requested())
            {
                ESP_LOGI(TAG, "电台让位窗口到期；直接归还扬声器恢复直播");
                s_session_exit_clean = true;
                close_channel(false);
            }
            else if (!s_followup_retry_used)
            {
                /* 第一段聆听窗口没等到人说话：先播一句“我没听清，可以再说一遍吗？”
                   （就是 prompt_timeout 那条资产），再开一轮窗口，其余逻辑不变。
                   提示音排空后 enter_followup_after_playback() 会自动重新装一个
                   XIAOZHI_FOLLOWUP_WINDOW_US 的窗口，这里不需要手工 arm，也不该
                   走 exit 分支——exit 分支会在播完后直接 close_channel。 */
                s_followup_retry_used = true;
                ESP_LOGI(TAG, "follow-up window silent; playing the retry prompt and re-arming once");
                if (!request_cloud_timeout_prompt())
                    play_error_tone_then_close(false);
            }
            else
            {
                ESP_LOGI(TAG, "follow-up window expired; requesting generated exit prompt");
                if (!request_cloud_exit_prompt())
                    play_error_tone_then_close(false);
            }
        }
        /* Never-audible watchdog: the turn was accepted but no PCM ever reached the
         * speaker, so the underrun watchdog below was never even armed. */
        if (s_tts_first_pcm_deadline_us != 0 &&
            (s_state == XIAOZHI_VOICE_TTS_BUFFERING ||
             s_state == XIAOZHI_VOICE_SPEAKING) &&
            esp_timer_get_time() >= s_tts_first_pcm_deadline_us)
        {
            s_tts_first_pcm_deadline_us = 0;
            ESP_LOGW(TAG, "TTS accepted but never became audible; finishing playback locally");
            xiaozhi_audio_pipeline_finish_playback();
            s_state = XIAOZHI_VOICE_TTS_DRAINING;
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
            /* Backstop for a deferred start whose turn already ended. */
            consume_deferred_media_starts();
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

/* 字符串字面量的地址不保证唯一，比 op 名称必须比内容。 */
static bool uplink_operation_equal(const char *left, const char *right)
{
    if (left == right) return true;
    if (left == NULL || right == NULL) return false;
    return strcmp(left, right) == 0;
}

static void websocket_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    const uint32_t connection_seq = (uint32_t)(uintptr_t)handler_args;
    (void)base;
    /* 这条日志原来是每个 WSS 事件打一条，而 WEBSOCKET_EVENT_DATA 在 TTS 期间
       每 40~90 ms 就来一次——实测 247 行的日志里它独占 52 行（21%），把
       "TTS packet arrival"、"late TTS packet"、"cloud JSON received" 这些真信号
       全淹了，每次排查都得先手动 grep。
       现在只有「不是 DATA」或「五元组变了」才打：状态跃迁一条不漏，稳态的数据流
       静默。格式串刻意保持原样——test_voice_connection_lifetime_contract.ps1 用
       不跨行的 .* 匹配 event_id / connection_seq / terminal_gen 三段。 */
    static int32_t last_event_id = INT32_MIN;
    static uint32_t last_connection_seq = UINT32_MAX;
    static const char *last_operation = NULL;
    static unsigned last_terminal_generation = UINT32_MAX;
    static int last_terminal_active = -1;
    /* volatile 量先快照再比较，否则几次读取之间可能变，日志会自相矛盾。 */
    const int terminal_active = s_uplink_terminal_active ? 1 : 0;
    const unsigned terminal_generation = s_uplink_terminal_generation;
    const bool state_changed =
        event_id != last_event_id || connection_seq != last_connection_seq ||
        !uplink_operation_equal(last_operation, s_uplink_tx_operation) ||
        terminal_generation != last_terminal_generation ||
        terminal_active != last_terminal_active;
    if (event_id != WEBSOCKET_EVENT_DATA || state_changed)
    {
        ESP_LOGI(TAG, "WSS callback event_id=%ld connection_seq=%u op=%s terminal_gen=%u terminal=%d",
                 (long)event_id, (unsigned)connection_seq, s_uplink_tx_operation,
                 terminal_generation, terminal_active);
        last_event_id = event_id;
        last_connection_seq = connection_seq;
        last_operation = s_uplink_tx_operation;
        last_terminal_generation = terminal_generation;
        last_terminal_active = terminal_active;
    }
    if (event_id == WEBSOCKET_EVENT_CONNECTED)
    {
        s_wss_last_connected_us = esp_timer_get_time();
        s_wss_last_ping_us = 0;
        s_wss_last_pong_us = 0;
        s_wss_last_rx_us = s_wss_last_connected_us;
        s_wss_health = XIAOZHI_WSS_HEALTH_HEALTHY;
        ESP_LOGI(TAG, "WSS heartbeat connected: seq=%u ping_interval_sec=10 pong_timeout_sec=20",
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
                ESP_LOGW(TAG, "WSS heartbeat timeout: no PONG within 20 seconds");
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
    case XIAOZHI_LOCAL_PROMPT_PLAYBACK_FINISHED:
        post_playback_event(VOICE_EVENT_LOCAL_PROMPT_FINISHED, epoch);
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
