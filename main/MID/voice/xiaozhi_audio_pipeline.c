#include "xiaozhi_audio_pipeline.h"

#include <math.h>
#include <string.h>

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio_playback_service.h"
#include "i2s_audio_driver.h"
#include "radio_stream_service.h"

#define XIAOZHI_TTS_QUEUE_BYTES (24U * 1024U)
#define XIAOZHI_TTS_PACKET_MAX_BYTES 2048U
/* TTS 开播门槛帧数（每帧 30ms）。与 audio_playback_service 的
 * AUDIO_PLAYOUT_TTS_PREBUFFER_FRAMES 保持一致。
 *
 * 50 帧（1.5s）。原来这里写“抗顿能力由更深的播放队列提供”，这句话是错的：
 * 开播后队列不会自动积满到 AUDIO_PLAYOUT_QUEUE_DEPTH=256U（供速≈实时），开播
 * 那一刻的水位就是整轮唯一的抖动缓冲——实测只用 18/32 帧（540 ms）就开播，
 * 510 ms 后就 underrun。本值直接决定首字延迟，所以它是一次取舍：1.5 s 换掉
 * 大部分卡顿，再深就要拿响应速度换了。 */
#define XIAOZHI_TTS_PREBUFFER_FRAMES 50U
#define XIAOZHI_AFE_RINGBUF_FRAMES 50U
/* AFE 诊断心跳间隔。这两条原来是 1 秒一条：实测一份 247 行的日志里
 * "AFE input" 占 32 行、"AFE state" 占 31 行，加起来 25%，把 TTS 和电台的关键行
 * 全淹了，每次排查都得先手动 grep 才看得见信号。现在改成：
 *   AFE state —— 任一字段变了立刻打，稳态（wake=0 vad=0 data=1024 cache=0）
 *                只在心跳时出现。它本来就是状态日志，原地重复没有信息量。
 *   AFE input —— 纯电平日志，心跳粒度足够看出底噪趋势；真正需要“越过门限那一刻”
 *                的场景由 VAD 那几条独立的门限日志负责，不会漏。
 * 契约测试钉的是格式串和 last_*_diagnostic_us 这两个限频变量本身，不受间隔影响。 */
#define AFE_DIAGNOSTIC_INTERVAL_US (5LL * 1000LL * 1000LL)
/* WakeNet9 allocates a few small objects from internal RAM while it builds its
 * model index and never checks those allocations: get_flash_index() passes a
 * NULL semaphore straight to xQueueSemaphoreTake(), which asserts and aborts the
 * firmware. A starved internal heap therefore reboots the device instead of
 * returning an error, and the boot never produces a usable log. Refuse the
 * creation ourselves while it is still recoverable. A healthy boot reaches this
 * point with a largest internal block around 7 KB, while the aborted boot was
 * down to 1 KB, so the floor sits well inside that gap. */
#define XIAOZHI_AFE_MIN_INTERNAL_BLOCK 3072U
/* 队列要装得下整段起手预卷（48 x 32 ms ≈ 26 个 60 ms 编码帧）再加上实时帧的
 * 周转余量：预卷是在 START_STREAM 那一刻一次性倒进来的，而 detect 任务的优先级
 * 比编码任务高，倒的时候编码任务基本抢不到 CPU，队列太浅
 * queue_complete_pcm_frame 就会从最旧的开始丢，正好把要救的起手丢掉。
 * PSRAM 分配，不吃内部 RAM。 */
#define PCM_FRAME_QUEUE_DEPTH 32U
#define PCM_FRAME_MAX_BYTES 2048U
/* 起手预卷容量：48 帧 x 32 ms = 1536 ms（16 kHz mono s16le，48 KB PSRAM）。
 * 真正要盖住的是「用户开口 -> 防抖凑满 4 帧」这一段：补发窗口永远含确认时刻，
 * 所以开口点到确认点的距离才是关键。实测最坏约 750 ms，而门限调到 800 之后
 * 这道门更稳、这段只会更长，留到 1.5 s 换足余量。代价是补发时连开口前的
 * 静音一起送上去，最坏多等约 1.5 s，XIAOZHI_WAIT_RESPONSE_TIMEOUT_US(15 s)
 * 装得下。 */
#define XIAOZHI_PREROLL_BYTES 49152U
#define AUDIO_CONTROL_QUEUE_DEPTH 8U
#define VOICE_CODEC_STACK_BYTES (32U * 1024U)
#define TTS_DECODE_LOG_INTERVAL_US (1000000LL)

/* Energy gate against noise-triggered VAD starts. WebRTC VAD in MODE_0 is
 * deliberately permissive because the ES8311 input level is low, so a VAD
 * start is reported only when the peak of the last ~300 ms of microphone
 * input clears a floor. Real speech clears it by a wide margin while the
 * measured noise floor stays below it. Recalibrate this floor with the
 * "AFE input: peak=" logs after the codec mic gain change takes effect:
 * keep it at 2-3x the quiet-room peak. Field logs on this unit showed the
 * ambient noise peaks reaching ~860 (838/863 observed) while real speech
 * stays well above 1500 (VAD_DIRECT_REPLY_PEAK).
 *
 * 门限从 800 降到 644（用户实机决定）。降门限换的是灵敏度：开口的第一个字更容易
 * 直接开门，起手预卷要补的那段就更短，被剪掉开头的概率更低。代价是 838/863 那种
 * 环境噪声峰现在也能踩进来——挡住它的是下面 4 帧的防抖 (VAD_START_CONFIRM_FRAMES)：
 * 单发尖峰撑不过连续 4 个 feed chunk。 */
#define VAD_START_MIN_RECENT_PEAK 644U
#define VAD_DIRECT_REPLY_PEAK 1500U
#define VAD_RECENT_PEAK_SAMPLES 10U /* 10 x 32 ms feed chunks >= 300 ms window */
/* Debounce against noise spikes: a VAD start is confirmed only after this
 * many consecutive feed chunks whose current-frame peak clears the energy
 * floor. A transient spike (1-3 chunks) resets the counter and is discarded,
 * while real speech (hundreds of ms) always satisfies it; 4 chunks also
 * rejects the measured multi-frame noise bursts. */
#define VAD_START_CONFIRM_FRAMES 4U

typedef enum {
    VOICE_AUDIO_CONTROL_START_STREAM = 0,
    VOICE_AUDIO_CONTROL_STOP_STREAM,
    VOICE_AUDIO_CONTROL_ARM_FOLLOWUP,
    VOICE_AUDIO_CONTROL_RESET_AFTER_SILENCE,
    VOICE_AUDIO_CONTROL_RESUME_AFTER_PLAYBACK,
    VOICE_AUDIO_CONTROL_PAUSE_DETECT,
} voice_audio_control_t;

typedef struct {
    uint16_t length;
    uint32_t epoch;
    uint8_t pcm[PCM_FRAME_MAX_BYTES];
} pcm_frame_t;

typedef struct {
    uint32_t epoch;
    size_t length;
    uint8_t data[];
} tts_packet_t;

typedef struct {
    const esp_afe_sr_iface_t *afe;
    esp_afe_sr_data_t *afe_data;
    esp_audio_enc_handle_t encoder;
    esp_audio_dec_handle_t decoder;
    QueueHandle_t pcm_frames;
    QueueHandle_t controls;
    RingbufHandle_t tts_queue;
    SemaphoreHandle_t playback_lock;
    pcm_frame_t *pcm_submit;
    pcm_frame_t *pcm_dropped;
    uint8_t *codec_opus;
    uint8_t *decode_pcm;
    pcm_frame_t *codec_frame;
    TaskHandle_t feed_task;
    TaskHandle_t detect_task;
    TaskHandle_t codec_task;
    xiaozhi_wake_word_cb_t wake_cb;
    xiaozhi_vad_state_cb_t vad_cb;
    xiaozhi_opus_frame_cb_t opus_cb;
    xiaozhi_tts_playback_cb_t playback_cb;
    xiaozhi_capture_complete_cb_t capture_complete_cb;
    void *callback_context;
    volatile bool initialized;
    volatile bool codec_ready;
    volatile bool codec_failed;
    volatile bool streaming;
    volatile bool detect_paused;
    volatile bool wake_latched;
    /* 设备自己正在出声（本地提示音 / 云端 TTS）。出声期间不采集起手预卷，
       否则补发时会把设备自己的话音当成用户刚说的话送上去。 */
    volatile bool playout_active;
    volatile uint32_t stream_epoch;
    volatile uint32_t playback_epoch;
    volatile bool playback_discard_pending;
    volatile bool tts_input_closed;
    volatile bool tts_finish_submitted;
    volatile uint32_t silence_frames_pending;
    volatile uint32_t silence_frames_epoch;
    vad_state_t last_vad_state;
    bool vad_high_energy_notified;
    uint8_t pcm_partial[PCM_FRAME_MAX_BYTES];
    /* 起手预卷：VAD 确认之前 AFE 输出的那一段音频。 */
    uint8_t *preroll_buffer;
    size_t preroll_len;
    size_t pcm_partial_len;
    size_t opus_pcm_bytes;
    size_t opus_packet_bytes;
    int64_t last_pcm_log_us;
    int64_t last_codec_log_us;
    uint32_t tts_packet_boundary_mismatches;
    uint32_t tts_decode_failure_count;
    uint32_t tts_queue_full_count;
    bool tts_format_logged;
    uint32_t capture_frames;
    uint32_t capture_bytes;
    uint32_t capture_cache_bytes;
    uint64_t capture_energy;
    uint32_t capture_peak;
    int64_t capture_started_us;
    /* Sliding window of recent feed peaks used to gate noise-triggered VAD
     * starts; only the detect task reads it, only the feed task writes it. */
    uint32_t vad_peak_window[VAD_RECENT_PEAK_SAMPLES];
    uint32_t vad_peak_index;
    /* Debounce counter: consecutive feed chunks whose current-frame peak clears
     * the energy floor. Reset on any sub-threshold chunk so a lone noise spike
     * cannot confirm a VAD start. Uses the instantaneous chunk peak rather
     * than the sliding-window max, because a single spike pollutes the window
     * max for ~300 ms and would otherwise fake a sustained high-energy run. */
    uint16_t vad_high_energy_frames;
    /* Latest instantaneous chunk peak, written by the feed task and read by
     * the detect task to drive the debounce counter above. */
    volatile uint32_t feed_peak_last;
} xiaozhi_audio_pipeline_state_t;

static const char *TAG = "xiaozhi_audio";
static int64_t s_tts_decode_log_us;
static xiaozhi_audio_pipeline_state_t s_pipeline;

static void log_memory(const char *stage)
{
    ESP_LOGI(TAG, "%s: internal=%u largest=%u psram=%u psram_largest=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static void discard_tts_queue(void)
{
    for (;;) {
        size_t item_size = 0;
        void *item = xRingbufferReceive(s_pipeline.tts_queue, &item_size, 0);
        if (item == NULL) return;
        vRingbufferReturnItem(s_pipeline.tts_queue, item);
    }
}

static void discard_pcm_frames(void)
{
    while (xQueueReceive(s_pipeline.pcm_frames, s_pipeline.pcm_dropped, 0) == pdTRUE) {
    }
}

static void vad_peak_window_clear(void)
{
    memset(s_pipeline.vad_peak_window, 0, sizeof(s_pipeline.vad_peak_window));
    s_pipeline.vad_peak_index = 0U;
    s_pipeline.vad_high_energy_frames = 0U;
}

static void vad_peak_window_push(uint32_t peak)
{
    s_pipeline.vad_peak_window[s_pipeline.vad_peak_index % VAD_RECENT_PEAK_SAMPLES] = peak;
    ++s_pipeline.vad_peak_index;
}

static uint32_t vad_peak_window_max(void)
{
    uint32_t peak = 0U;
    for (uint32_t index = 0U; index < VAD_RECENT_PEAK_SAMPLES; ++index) {
        if (s_pipeline.vad_peak_window[index] > peak) peak = s_pipeline.vad_peak_window[index];
    }
    return peak;
}

static void append_afe_pcm(const uint8_t *data, size_t bytes);

/* 起手预卷。能量门限和防抖必须留着（实测环境噪声经常越过 500，800 这道门才
 * 稳），代价是用户开口的头一段会被判成噪声丢掉：实测日志 20.24 s 开口、
 * 21.28 s 才确认，“关闭电台”的“关闭”两个字整段没上云，云端只好拿着
 * “电台。”自己去猜动词。这段音频在 AFE 输出里一直是连续的，只是 streaming
 * 还没打开。这里先把它留住，确认时一次性补发。 */
static void preroll_reset(void)
{
    s_pipeline.preroll_len = 0U;
    /* playout_active 只可能在 audio_playback_service_abort() 之后卡住（abort
       不发事件），而每一个窗口边界都会走到这里，所以顺手复位，免得预卷永久失效。 */
    s_pipeline.playout_active = false;
}

static void preroll_push(const uint8_t *data, size_t bytes)
{
    if (s_pipeline.preroll_buffer == NULL || data == NULL || bytes == 0U) return;
    if (bytes >= XIAOZHI_PREROLL_BYTES) {
        /* 一帧就超过整个缓冲：只留最后一段。 */
        memcpy(s_pipeline.preroll_buffer, data + (bytes - XIAOZHI_PREROLL_BYTES),
               XIAOZHI_PREROLL_BYTES);
        s_pipeline.preroll_len = XIAOZHI_PREROLL_BYTES;
        return;
    }
    if (s_pipeline.preroll_len + bytes > XIAOZHI_PREROLL_BYTES) {
        /* 溢出多少就从最旧的丢多少，缓冲里始终是一段连续的音频，不做环形折返。 */
        const size_t drop = s_pipeline.preroll_len + bytes - XIAOZHI_PREROLL_BYTES;
        memmove(s_pipeline.preroll_buffer, s_pipeline.preroll_buffer + drop,
                s_pipeline.preroll_len - drop);
        s_pipeline.preroll_len -= drop;
    }
    memcpy(s_pipeline.preroll_buffer + s_pipeline.preroll_len, data, bytes);
    s_pipeline.preroll_len += bytes;
}

/* 把预卷按 60 ms 的帧交给编码任务，余数留在 pcm_partial 里由实时数据补齐。
 * 调用点必须在 ++stream_epoch 之后，这些帧才会带上本轮的 epoch。 */
static void preroll_drain(void)
{
    if (s_pipeline.preroll_len > 0U) {
        ESP_LOGI(TAG, "preroll replay: bytes=%u ms=%u",
                 (unsigned)s_pipeline.preroll_len, (unsigned)(s_pipeline.preroll_len / 32U));
        append_afe_pcm(s_pipeline.preroll_buffer, s_pipeline.preroll_len);
    }
    s_pipeline.preroll_len = 0U;
}

static void apply_audio_controls(void)
{
    voice_audio_control_t control;
    while (xQueueReceive(s_pipeline.controls, &control, 0) == pdTRUE) {
        switch (control) {
        case VOICE_AUDIO_CONTROL_START_STREAM:
            s_pipeline.pcm_partial_len = 0U;
            s_pipeline.silence_frames_pending = 0U;
            s_pipeline.wake_latched = true;
            s_pipeline.capture_frames = 0U;
            s_pipeline.capture_bytes = 0U;
            s_pipeline.capture_cache_bytes = 0U;
            s_pipeline.capture_energy = 0U;
            s_pipeline.capture_peak = 0U;
            s_pipeline.capture_started_us = esp_timer_get_time();
            s_pipeline.last_vad_state = VAD_SILENCE;
            s_pipeline.vad_high_energy_notified = false;
            discard_pcm_frames();
            if (s_pipeline.afe != NULL && s_pipeline.afe_data != NULL) {
                s_pipeline.afe->reset_buffer(s_pipeline.afe_data);
            }
            ++s_pipeline.stream_epoch;
            s_pipeline.streaming = true;
            /* 起手预卷接在实时流前面：VAD 门限切掉的那段开头在这里补回来。 */
            preroll_drain();
            ESP_LOGI(TAG, "voice stream start: epoch=%u", (unsigned)s_pipeline.stream_epoch);
            log_memory("before first stream frame");
            break;
        case VOICE_AUDIO_CONTROL_STOP_STREAM:
            preroll_reset();
            s_pipeline.detect_paused = false;
            s_pipeline.streaming = false;
            s_pipeline.wake_latched = false;
            s_pipeline.last_vad_state = VAD_SILENCE;
            s_pipeline.vad_high_energy_notified = false;
            vad_peak_window_clear();
            s_pipeline.pcm_partial_len = 0U;
            const int64_t capture_duration_us = s_pipeline.capture_started_us > 0
                                                    ? esp_timer_get_time() - s_pipeline.capture_started_us : 0;
            const uint32_t capture_rms = s_pipeline.capture_frames > 0U
                                             ? (uint32_t)sqrt((double)s_pipeline.capture_energy /
                                                              (double)s_pipeline.capture_frames /
                                                              (double)(s_pipeline.afe != NULL && s_pipeline.afe_data != NULL
                                                                          ? s_pipeline.afe->get_feed_chunksize(s_pipeline.afe_data) : 1))
                                             : 0U;
            ESP_LOGI(TAG, "capture diagnostics: duration_us=%lld frames=%u bytes=%u cache_bytes=%u rms=%u peak=%u",
                     (long long)capture_duration_us, (unsigned)s_pipeline.capture_frames,
                     (unsigned)s_pipeline.capture_bytes, (unsigned)s_pipeline.capture_cache_bytes,
                     (unsigned)capture_rms, (unsigned)s_pipeline.capture_peak);
            if (s_pipeline.capture_frames < 4U) {
                ESP_LOGW(TAG, "capture_short: frames=%u duration_us=%lld",
                         (unsigned)s_pipeline.capture_frames, (long long)capture_duration_us);
            }
            if (capture_rms < 100U) {
                ESP_LOGW(TAG, "capture_low_level: rms=%u peak=%u",
                         (unsigned)capture_rms, (unsigned)s_pipeline.capture_peak);
            }
            if (s_pipeline.capture_frames > 0U && s_pipeline.capture_complete_cb != NULL) {
                s_pipeline.capture_complete_cb(capture_rms, s_pipeline.capture_peak,
                                                s_pipeline.capture_frames, capture_duration_us,
                                                s_pipeline.callback_context);
            }
            if (s_pipeline.capture_peak >= 30000U) {
                ESP_LOGW(TAG, "capture_clipping_risk: peak=%u threshold=30000",
                         (unsigned)s_pipeline.capture_peak);
            }
            s_pipeline.capture_started_us = 0;
            s_pipeline.silence_frames_pending = 0U;
            ++s_pipeline.stream_epoch;
            discard_pcm_frames();
            if (s_pipeline.afe != NULL && s_pipeline.afe_data != NULL) {
                s_pipeline.afe->reset_buffer(s_pipeline.afe_data);
            }
            ESP_LOGI(TAG, "voice stream stopped: epoch=%u", (unsigned)s_pipeline.stream_epoch);
            break;
        case VOICE_AUDIO_CONTROL_ARM_FOLLOWUP:
            preroll_reset();
            s_pipeline.detect_paused = false;
            s_pipeline.streaming = false;
            s_pipeline.wake_latched = true;
            s_pipeline.last_vad_state = VAD_SILENCE;
            s_pipeline.vad_high_energy_notified = false;
            vad_peak_window_clear();
            s_pipeline.pcm_partial_len = 0U;
            ++s_pipeline.stream_epoch;
            discard_pcm_frames();
            if (s_pipeline.afe != NULL && s_pipeline.afe_data != NULL) {
                s_pipeline.afe->reset_buffer(s_pipeline.afe_data);
            }
            ESP_LOGI(TAG, "voice follow-up armed: epoch=%u", (unsigned)s_pipeline.stream_epoch);
            break;
        case VOICE_AUDIO_CONTROL_RESET_AFTER_SILENCE:
            /* 这里不能清预卷。follow-up 直接接话的路径是“重置基线 -> 立刻开流”：
               语音服务在 recent_peak 过了 FOLLOWUP_DIRECT_REPLY_MIN_PEAK 之后先发
               RESET_AFTER_SILENCE 再发 START_STREAM。实测日志 54490 的
               "voice AFE reset after silence" 后面紧跟 "voice stream start"，
               preroll replay 一次都没打出来过——刚攒的那 1 秒全被这里倒掉了，
               语音仍被剪成 "B电台。"。设备出声的窗口由 playout_active 挡着，
               不需要靠清缓冲来兜底。 */
            s_pipeline.detect_paused = false;
            s_pipeline.last_vad_state = VAD_SILENCE;
            s_pipeline.vad_high_energy_notified = false;
            vad_peak_window_clear();
            s_pipeline.pcm_partial_len = 0U;
            s_pipeline.silence_frames_pending = 0U;
            discard_pcm_frames();
            if (s_pipeline.afe != NULL && s_pipeline.afe_data != NULL) {
                s_pipeline.afe->reset_buffer(s_pipeline.afe_data);
            }
            ESP_LOGI(TAG, "voice AFE reset after silence");
            break;
        case VOICE_AUDIO_CONTROL_RESUME_AFTER_PLAYBACK:
            preroll_reset();
            s_pipeline.detect_paused = false;
            s_pipeline.streaming = false;
            s_pipeline.wake_latched = true;
            s_pipeline.last_vad_state = VAD_SILENCE;
            s_pipeline.vad_high_energy_notified = false;
            vad_peak_window_clear();
            s_pipeline.pcm_partial_len = 0U;
            discard_pcm_frames();
            if (s_pipeline.afe != NULL && s_pipeline.afe_data != NULL) {
                s_pipeline.afe->reset_buffer(s_pipeline.afe_data);
            }
            ESP_LOGI(TAG, "voice AFE reset after playout drain; fresh VAD baseline ready");
            break;
        case VOICE_AUDIO_CONTROL_PAUSE_DETECT:
            preroll_reset();
            s_pipeline.detect_paused = true;
            s_pipeline.streaming = false;
            s_pipeline.wake_latched = false;
            s_pipeline.last_vad_state = VAD_SILENCE;
            s_pipeline.vad_high_energy_notified = false;
            vad_peak_window_clear();
            s_pipeline.pcm_partial_len = 0U;
            s_pipeline.silence_frames_pending = 0U;
            discard_pcm_frames();
            if (s_pipeline.afe != NULL && s_pipeline.afe_data != NULL) {
                s_pipeline.afe->reset_buffer(s_pipeline.afe_data);
            }
            ESP_LOGI(TAG, "voice AFE detection paused during PCM playback");
            break;
        }
    }
}

static void queue_complete_pcm_frame(void)
{
    s_pipeline.pcm_submit->length = (uint16_t)s_pipeline.opus_pcm_bytes;
    s_pipeline.pcm_submit->epoch = s_pipeline.stream_epoch;
    memcpy(s_pipeline.pcm_submit->pcm, s_pipeline.pcm_partial, s_pipeline.opus_pcm_bytes);
    if (xQueueSend(s_pipeline.pcm_frames, s_pipeline.pcm_submit, 0) != pdTRUE) {
        (void)xQueueReceive(s_pipeline.pcm_frames, s_pipeline.pcm_dropped, 0);
        if (xQueueSend(s_pipeline.pcm_frames, s_pipeline.pcm_submit, 0) != pdTRUE) {
            ESP_LOGW(TAG, "PCM frame dropped: queue unavailable");
            return;
        }
        ESP_LOGW(TAG, "PCM frame queue full; dropped oldest realtime frame");
    }
    const int64_t now_us = esp_timer_get_time();
    if (now_us - s_pipeline.last_pcm_log_us >= 1000000LL) {
        ESP_LOGI(TAG, "PCM frame queued: bytes=%u epoch=%u detect_stack=%u",
                 (unsigned)s_pipeline.pcm_submit->length, (unsigned)s_pipeline.pcm_submit->epoch,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        s_pipeline.last_pcm_log_us = now_us;
    }
}

static void append_afe_pcm(const uint8_t *data, size_t bytes)
{
    while (bytes > 0U) {
        const size_t remaining = s_pipeline.opus_pcm_bytes - s_pipeline.pcm_partial_len;
        const size_t copied = bytes < remaining ? bytes : remaining;
        memcpy(s_pipeline.pcm_partial + s_pipeline.pcm_partial_len, data, copied);
        s_pipeline.pcm_partial_len += copied;
        data += copied;
        bytes -= copied;
        if (s_pipeline.pcm_partial_len == s_pipeline.opus_pcm_bytes) {
            queue_complete_pcm_frame();
            s_pipeline.pcm_partial_len = 0U;
        }
    }
}

static void audio_feed_task(void *arg)
{
    (void)arg;
    const int chunks = s_pipeline.afe->get_feed_chunksize(s_pipeline.afe_data);
    const int channels = s_pipeline.afe->get_feed_channel_num(s_pipeline.afe_data);
    const size_t bytes = (size_t)chunks * (size_t)channels * sizeof(int16_t);
    int16_t *microphone = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (chunks <= 0 || channels <= 0 || microphone == NULL) {
        ESP_LOGE(TAG, "voice feed allocation/configuration failed");
        vTaskSuspend(NULL);
        return;
    }
    int64_t next_us = esp_timer_get_time();
    int64_t last_input_diagnostic_us = 0;
    ESP_LOGI(TAG, "audio_feed_task started: samples=%d channels=%d bytes=%u", chunks, channels,
             (unsigned)bytes);
    for (;;) {
        if (i2s_audio_driver_record((uint8_t *)microphone, bytes, NULL) == ESP_OK) {
            /* Compute one pass of energy/peak for every chunk so the VAD
             * energy gate sees fresh input even when capture is paused. */
            uint64_t feed_energy = 0U;
            uint32_t feed_peak = 0U;
            const size_t samples = bytes / sizeof(*microphone);
            for (size_t index = 0; index < samples; ++index) {
                const int32_t sample = microphone[index];
                const uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);
                feed_energy += (uint64_t)magnitude * magnitude;
                if (magnitude > feed_peak) feed_peak = magnitude;
            }
            vad_peak_window_push(feed_peak);
            s_pipeline.feed_peak_last = feed_peak;
            if (s_pipeline.streaming) {
                ++s_pipeline.capture_frames;
                s_pipeline.capture_bytes += (uint32_t)bytes;
                s_pipeline.capture_energy += feed_energy;
                if (feed_peak > s_pipeline.capture_peak) s_pipeline.capture_peak = feed_peak;
            }
            const int accepted = s_pipeline.afe->feed(s_pipeline.afe_data, microphone);
            const int64_t now_us = esp_timer_get_time();
            if (now_us - last_input_diagnostic_us >= AFE_DIAGNOSTIC_INTERVAL_US) {
                ESP_LOGI(TAG, "AFE input: rms=%u peak=%u feed=%d/%u",
                         (unsigned)sqrt((double)feed_energy / (double)samples),
                         (unsigned)feed_peak, accepted, (unsigned)bytes);
                last_input_diagnostic_us = now_us;
            }
        }
        next_us += ((int64_t)chunks * 1000000LL + LUMIDOCK_AUDIO_SAMPLE_RATE_HZ - 1LL) /
                   LUMIDOCK_AUDIO_SAMPLE_RATE_HZ;
        const int64_t now_us = esp_timer_get_time();
        if (next_us <= now_us) next_us = now_us;
        else vTaskDelay(pdMS_TO_TICKS((uint32_t)((next_us - now_us + 999LL) / 1000LL)));
    }
}

static void audio_detect_task(void *arg)
{
    (void)arg;
    bool first_frame = true;
    int64_t last_afe_diagnostic_us = 0;
    /* 上一份打出去的状态，用来判断“变了没有”。-1 保证第一帧一定打一条。 */
    int last_diag_wake = -1;
    int last_diag_vad = -1;
    int last_diag_data = -1;
    int last_diag_cache = -1;
    ESP_LOGI(TAG, "audio_detect_task started");
    for (;;) {
        apply_audio_controls();
        afe_fetch_result_t *result = s_pipeline.afe->fetch(s_pipeline.afe_data);
        if (result == NULL || result->ret_value == ESP_FAIL) continue;
        if (first_frame) {
            first_frame = false;
            ESP_LOGI(TAG, "audio_detect_task received first AFE frame: bytes=%d", result->data_size);
        }
        const int64_t now_us = esp_timer_get_time();
        /* 显式转 int：wakeup_state 的枚举里有 -1（有符号），vad_state 全非负
           （GCC 可能选 unsigned 做底层类型），不转会在 -Wextra 下吃 -Wsign-compare。 */
        const bool diagnostic_changed = (int)result->wakeup_state != last_diag_wake ||
                                        (int)result->vad_state != last_diag_vad ||
                                        result->data_size != last_diag_data ||
                                        result->vad_cache_size != last_diag_cache;
        if (diagnostic_changed || now_us - last_afe_diagnostic_us >= AFE_DIAGNOSTIC_INTERVAL_US) {
            ESP_LOGI(TAG, "AFE state: wake=%d vad=%d data=%d cache=%d detect_stack=%u",
                     result->wakeup_state, result->vad_state, result->data_size, result->vad_cache_size,
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            last_afe_diagnostic_us = now_us;
            last_diag_wake = (int)result->wakeup_state;
            last_diag_vad = (int)result->vad_state;
            last_diag_data = result->data_size;
            last_diag_cache = result->vad_cache_size;
        }
        /* Wake word stays armed during TTS playback so the user can abort a
         * long answer.  Wakenet applies its own post-detection cooldown, so a
         * single detection cannot re-trigger the callback continuously.
         * VAD below still pauses during playback to keep the speaker echo
         * from self-triggering speech capture. */
        if (result->wakeup_state == WAKENET_DETECTED) {
            s_pipeline.wake_latched = true;
            s_pipeline.last_vad_state = VAD_SILENCE;
            s_pipeline.vad_high_energy_notified = false;
            if (s_pipeline.wake_cb != NULL) s_pipeline.wake_cb(s_pipeline.callback_context);
        }
        if (s_pipeline.detect_paused) continue;
        if (!s_pipeline.wake_latched) continue;
        const bool vad_speech = result->vad_state == VAD_SPEECH;
        const uint32_t recent_peak = vad_speech ? vad_peak_window_max() : 0U;

        /* 起手预卷：VAD 还没确认、能量门限还在拦的时候，AFE 的输出照样是连续
           的语音，先留住。必须放在下面几条 continue 之前——被门限挡掉的那些
           帧正好就是要补回来的那一段。streaming 一开就停，免得重复采集。 */
        if (!s_pipeline.streaming && !s_pipeline.playout_active && result->data_size > 0) {
            preroll_push((const uint8_t *)result->data, (size_t)result->data_size);
        }

        /* Debounce noise-triggered starts using the current frame's peak, not
         * the sliding-window max (a lone spike would pollute the window max for
         * ~300 ms and fake a sustained run). A transient spike resets the
         * counter; real speech always spans enough chunks to confirm. */
        if (vad_speech && s_pipeline.feed_peak_last >= VAD_START_MIN_RECENT_PEAK) {
            if (s_pipeline.vad_high_energy_frames < VAD_START_CONFIRM_FRAMES) {
                ++s_pipeline.vad_high_energy_frames;
            }
        } else {
            s_pipeline.vad_high_energy_frames = 0U;
        }
        const bool vad_confirmed = vad_speech &&
                                   s_pipeline.vad_high_energy_frames >= VAD_START_CONFIRM_FRAMES;

        if (result->vad_state != s_pipeline.last_vad_state) {
            if (vad_speech && recent_peak < VAD_START_MIN_RECENT_PEAK) {
                /* Keep last_vad_state unchanged so a louder burst later
                 * in the same speech segment can still open the gate. */
                ESP_LOGI(TAG, "VAD start gated by energy: recent_peak=%u threshold=%u",
                         (unsigned)recent_peak, (unsigned)VAD_START_MIN_RECENT_PEAK);
                continue;
            }
            if (vad_speech && !vad_confirmed) {
                ESP_LOGI(TAG, "VAD start gated by debounce: frames=%u/%u peak=%u",
                         (unsigned)s_pipeline.vad_high_energy_frames,
                         (unsigned)VAD_START_CONFIRM_FRAMES,
                         (unsigned)s_pipeline.feed_peak_last);
                continue;
            }
            s_pipeline.last_vad_state = result->vad_state;
            s_pipeline.vad_high_energy_notified = vad_speech &&
                                                   recent_peak >= VAD_DIRECT_REPLY_PEAK;
            if (s_pipeline.vad_cb != NULL) {
                s_pipeline.vad_cb(result->vad_state == VAD_SPEECH, recent_peak,
                                  s_pipeline.callback_context);
            }
        } else if (vad_speech && !s_pipeline.vad_high_energy_notified &&
                   recent_peak >= VAD_DIRECT_REPLY_PEAK) {
            /* Echo may put AFE into VAD_SPEECH before the user begins
             * talking. Emit one high-energy edge so the post-TTS gate can
             * accept real speech without waiting for that echo to end. */
            s_pipeline.vad_high_energy_notified = true;
            if (s_pipeline.vad_cb != NULL) {
                s_pipeline.vad_cb(true, recent_peak, s_pipeline.callback_context);
            }
        }
        if (!s_pipeline.streaming || result->vad_state != VAD_SPEECH) continue;
        if (result->vad_cache_size > 0) {
            s_pipeline.capture_cache_bytes += (uint32_t)result->vad_cache_size;
            append_afe_pcm((const uint8_t *)result->vad_cache, (size_t)result->vad_cache_size);
        }
        if (result->data_size > 0) {
            append_afe_pcm((const uint8_t *)result->data, (size_t)result->data_size);
        }
    }
}

static void playout_event(audio_playback_event_t event, audio_playback_source_t source,
                          uint32_t epoch, void *context)
{
    (void)context;
    /* 设备出声的窗口：本地提示音和云端 TTS 都经过这里。起手预卷要避开它，
       否则“下次再聊”这类提示音会被当成用户刚说的话补发上去。 */
    if (event == AUDIO_PLAYOUT_EVENT_STARTED) s_pipeline.playout_active = true;
    else if (event == AUDIO_PLAYOUT_EVENT_FINISHED) s_pipeline.playout_active = false;
    if (source == AUDIO_PLAYOUT_SOURCE_ERROR_TONE) {
        /* The local error tone is not TTS: only its completion matters, and
         * it is forwarded so the voice service can close the failed turn. */
        if (event == AUDIO_PLAYOUT_EVENT_FINISHED && s_pipeline.playback_cb != NULL) {
            s_pipeline.playback_cb(XIAOZHI_ERROR_TONE_PLAYBACK_FINISHED, epoch,
                                   s_pipeline.callback_context);
        }
        return;
    }
    if (source == AUDIO_PLAYOUT_SOURCE_LOCAL_PROMPT) {
        /* A local intent confirmation prompt is never TTS; its completion is
         * forwarded so the voice service can release the queued cloud reply. */
        if (event == AUDIO_PLAYOUT_EVENT_FINISHED && s_pipeline.playback_cb != NULL) {
            s_pipeline.playback_cb(XIAOZHI_LOCAL_PROMPT_PLAYBACK_FINISHED, epoch,
                                   s_pipeline.callback_context);
        }
        return;
    }
    if (source != AUDIO_PLAYOUT_SOURCE_TTS || s_pipeline.playback_cb == NULL) return;
    xiaozhi_tts_playback_event_t pipeline_event = XIAOZHI_TTS_PLAYBACK_STARTED;
    if (event == AUDIO_PLAYOUT_EVENT_FINISHED) pipeline_event = XIAOZHI_TTS_PLAYBACK_FINISHED;
    else if (event == AUDIO_PLAYOUT_EVENT_UNDERRUN) pipeline_event = XIAOZHI_TTS_PLAYBACK_UNDERRUN;
    s_pipeline.playback_cb(pipeline_event, epoch, s_pipeline.callback_context);
}

static bool decode_one_tts_packet(uint8_t *pcm, size_t pcm_capacity)
{
    size_t packet_size = 0;
    tts_packet_t *packet = xRingbufferReceive(s_pipeline.tts_queue, &packet_size, 0);
    if (packet == NULL) return false;
    if (packet_size < sizeof(*packet) || packet->length < 5U ||
        packet->length > XIAOZHI_TTS_PACKET_MAX_BYTES ||
        packet_size != sizeof(*packet) + packet->length) {
        ESP_LOGW(TAG, "TTS packet rejected: item=%u payload=%u", (unsigned)packet_size,
                 (unsigned)packet->length);
        vRingbufferReturnItem(s_pipeline.tts_queue, packet);
        return true;
    }

    if (packet->epoch != s_pipeline.playback_epoch) {
        vRingbufferReturnItem(s_pipeline.tts_queue, packet);
        return true;
    }

    esp_audio_dec_in_raw_t input = {.buffer = packet->data, .len = (uint32_t)packet->length};
    esp_audio_dec_out_frame_t output = {.buffer = pcm, .len = (uint32_t)pcm_capacity, .decoded_size = 0U};
    esp_audio_dec_info_t info = {0};
    const int64_t decode_begin_us = esp_timer_get_time();
    const esp_audio_err_t decode_result = esp_opus_dec_decode(s_pipeline.decoder, &input, &output, &info);
    const int64_t decode_end_us = esp_timer_get_time();
    if (decode_result != ESP_AUDIO_ERR_OK || output.decoded_size == 0U || input.consumed == 0U) {
        ++s_pipeline.tts_decode_failure_count;
        ESP_LOGW(TAG, "TTS decode failed: bytes=%u consumed=%u err=%d",
                 (unsigned)packet->length, (unsigned)input.consumed, decode_result);
        ESP_LOGW(TAG, "TTS decode failure count=%u", (unsigned)s_pipeline.tts_decode_failure_count);
        vRingbufferReturnItem(s_pipeline.tts_queue, packet);
        return true;
    }
    if (input.consumed != packet->length) {
        ++s_pipeline.tts_packet_boundary_mismatches;
        ESP_LOGW(TAG, "TTS packet boundary mismatch: bytes=%u consumed=%u dropped_tail=%u count=%u",
                 (unsigned)packet->length, (unsigned)input.consumed,
                 (unsigned)(packet->length - input.consumed),
                 (unsigned)s_pipeline.tts_packet_boundary_mismatches);
    }
    if (!s_pipeline.tts_format_logged) {
        ESP_LOGI(TAG, "tts decode format: stream_rate=%u channels=%u bits=%u frame=%u pcm=%u",
                 (unsigned)info.sample_rate, (unsigned)info.channel,
                 (unsigned)info.bits_per_sample, (unsigned)info.frame_size,
                 (unsigned)output.decoded_size);
        s_pipeline.tts_format_logged = true;
    }
    int64_t playout_enqueue_us = 0;
    esp_err_t play_result = ESP_OK;
    if (packet->epoch == s_pipeline.playback_epoch) {
        const int64_t playout_begin_us = esp_timer_get_time();
        play_result = audio_playback_service_enqueue_pcm(AUDIO_PLAYOUT_SOURCE_TTS, packet->epoch,
                                                         output.buffer, output.decoded_size);
        playout_enqueue_us = esp_timer_get_time() - playout_begin_us;
    }
    const int64_t now_us = esp_timer_get_time();
    if (now_us - s_tts_decode_log_us >= TTS_DECODE_LOG_INTERVAL_US || play_result != ESP_OK) {
        ESP_LOGI(TAG, "TTS decode timing: bytes=%u consumed=%u decode_us=%lld playout_enqueue_us=%lld play_err=%s",
                 (unsigned)packet->length, (unsigned)input.consumed,
                 (long long)(decode_end_us - decode_begin_us), (long long)playout_enqueue_us,
                 esp_err_to_name(play_result));
        s_tts_decode_log_us = now_us;
    }
    vRingbufferReturnItem(s_pipeline.tts_queue, packet);
    return true;
}

static void voice_codec_task(void *arg)
{
    (void)arg;
    int pcm_bytes = 0;
    int opus_bytes = 0;
    if (esp_opus_enc_open(&(esp_opus_enc_config_t){
                              .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
                              .channel = ESP_AUDIO_MONO,
                              .bits_per_sample = ESP_AUDIO_BIT16,
                              .bitrate = 32000,
                              .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
                              .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,
                              .complexity = 5,
                          }, sizeof(esp_opus_enc_config_t), &s_pipeline.encoder) != ESP_AUDIO_ERR_OK ||
        esp_opus_enc_get_frame_size(s_pipeline.encoder, &pcm_bytes, &opus_bytes) != ESP_AUDIO_ERR_OK ||
        pcm_bytes <= 0 || (size_t)pcm_bytes > PCM_FRAME_MAX_BYTES || opus_bytes <= 0) {
        ESP_LOGE(TAG, "voice codec encoder setup failed");
        s_pipeline.codec_failed = true;
        vTaskSuspend(NULL);
        return;
    }
    s_pipeline.opus_pcm_bytes = (size_t)pcm_bytes;
    s_pipeline.opus_packet_bytes = (size_t)opus_bytes;
    if (esp_opus_dec_open(&(esp_opus_dec_cfg_t){
                              .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
                              .channel = ESP_AUDIO_MONO,
                              .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS,
                              .self_delimited = false,
                          }, sizeof(esp_opus_dec_cfg_t), &s_pipeline.decoder) != ESP_AUDIO_ERR_OK ||
        (s_pipeline.codec_opus = heap_caps_malloc(s_pipeline.opus_packet_bytes,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) == NULL ||
        (s_pipeline.decode_pcm = heap_caps_malloc(8U * 1024U,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) == NULL) {
        ESP_LOGE(TAG, "voice codec decoder/buffer setup failed");
        s_pipeline.codec_failed = true;
        vTaskSuspend(NULL);
        return;
    }
    s_pipeline.codec_frame = heap_caps_malloc(sizeof(*s_pipeline.codec_frame),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_pipeline.codec_frame == NULL) {
        ESP_LOGE(TAG, "voice codec PCM frame allocation failed");
        s_pipeline.codec_failed = true;
        vTaskSuspend(NULL);
        return;
    }
    s_pipeline.codec_ready = true;
    ESP_LOGI(TAG, "voice codec ready: pcm=%u opus=%u codec_stack=%u",
             (unsigned)s_pipeline.opus_pcm_bytes, (unsigned)s_pipeline.opus_packet_bytes,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    log_memory("after Opus codec creation");
    for (;;) {
        if (s_pipeline.playback_discard_pending) {
            discard_tts_queue();
            s_pipeline.playback_discard_pending = false;
            ESP_LOGI(TAG, "voice playback queue discarded by codec: epoch=%u",
                     (unsigned)s_pipeline.playback_epoch);
        }
        /* Wake primer silence is time-critical: generate it ahead of a busy
         * downlink decoder so the server's post-detect ignore window expires
         * while its wake greeting is still playing. */
        if (s_pipeline.silence_frames_pending == 0U &&
            decode_one_tts_packet(s_pipeline.decode_pcm, 8U * 1024U)) continue;
        if (s_pipeline.tts_input_closed && !s_pipeline.tts_finish_submitted) {
            s_pipeline.tts_finish_submitted = true;
            (void)audio_playback_service_finish(AUDIO_PLAYOUT_SOURCE_TTS,
                                                s_pipeline.playback_epoch);
            ESP_LOGI(TAG, "TTS input closed; decoded PCM tail is draining: epoch=%u",
                     (unsigned)s_pipeline.playback_epoch);
        }
        bool have_frame = false;
        bool silence_frame = false;
        if (s_pipeline.silence_frames_pending > 0U) {
            --s_pipeline.silence_frames_pending;
            memset(s_pipeline.codec_frame->pcm, 0, s_pipeline.opus_pcm_bytes);
            s_pipeline.codec_frame->length = (uint16_t)s_pipeline.opus_pcm_bytes;
            s_pipeline.codec_frame->epoch = s_pipeline.silence_frames_epoch;
            have_frame = true;
            silence_frame = true;
        } else if (xQueueReceive(s_pipeline.pcm_frames, s_pipeline.codec_frame,
                                pdMS_TO_TICKS(10)) == pdTRUE) {
            have_frame = true;
        }
        if (have_frame && s_pipeline.codec_frame->length == s_pipeline.opus_pcm_bytes &&
            (silence_frame || (s_pipeline.codec_frame->epoch == s_pipeline.stream_epoch &&
                               s_pipeline.streaming))) {
            const int64_t now_us = esp_timer_get_time();
            if (now_us - s_pipeline.last_codec_log_us >= 1000000LL) {
                ESP_LOGI(TAG, "Opus encode begin: bytes=%u codec_stack=%u",
                         (unsigned)s_pipeline.codec_frame->length,
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
            esp_audio_enc_in_frame_t input = {.buffer = s_pipeline.codec_frame->pcm,
                                               .len = s_pipeline.codec_frame->length};
            esp_audio_enc_out_frame_t output = {
                .buffer = s_pipeline.codec_opus,
                .len = (uint32_t)s_pipeline.opus_packet_bytes,
                .encoded_bytes = 0U,
            };
            output.encoded_bytes = 0U;
            const esp_audio_err_t result = esp_opus_enc_process(s_pipeline.encoder, &input, &output);
            if (result == ESP_AUDIO_ERR_OK && output.encoded_bytes > 0U &&
                output.encoded_bytes <= s_pipeline.opus_packet_bytes) {
                if (s_pipeline.opus_cb != NULL) {
                    s_pipeline.opus_cb(output.buffer, output.encoded_bytes, s_pipeline.callback_context);
                }
                if (now_us - s_pipeline.last_codec_log_us >= 1000000LL) {
                    ESP_LOGI(TAG, "Opus encode end: bytes=%u", (unsigned)output.encoded_bytes);
                    ESP_LOGI(TAG, "Opus packet queued: bytes=%u", (unsigned)output.encoded_bytes);
                    s_pipeline.last_codec_log_us = now_us;
                    log_memory("after first Opus frame");
                }
            } else {
                ESP_LOGW(TAG, "Opus encode rejected: err=%d bytes=%u capacity=%u",
                         result, (unsigned)output.encoded_bytes, (unsigned)s_pipeline.opus_packet_bytes);
            }
        }
    }
}

static esp_err_t init_afe(void)
{
    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGE(TAG, "ESP-SR init failure: model list unavailable");
        log_memory("ESP-SR model list failure");
        return ESP_ERR_NOT_FOUND;
    }
    afe_config_t *config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (config == NULL) {
        ESP_LOGE(TAG, "ESP-SR init failure: AFE config unavailable");
        log_memory("ESP-SR AFE config failure");
        return ESP_ERR_NO_MEM;
    }
    config->aec_init = false;
    config->ns_init = false;
    config->se_init = false;
    config->vad_init = true;
    /* ESP-SR defines lower modes as less restrictive. The measured ES8311
     * microphone level needs normal sensitivity for post-TTS questions. */
    config->vad_mode = VAD_MODE_0;
    config->vad_min_speech_ms = 128;
    config->vad_min_noise_ms = 1500;
    config->wakenet_init = true;
    config->wakenet_mode = DET_MODE_90;
    config->afe_ringbuf_size = XIAOZHI_AFE_RINGBUF_FRAMES;
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    config = afe_config_check(config);
    if (!config->wakenet_init) {
        ESP_LOGE(TAG, "ESP-SR init failure: WakeNet model unavailable");
        log_memory("ESP-SR WakeNet model failure");
        afe_config_free(config);
        return ESP_ERR_NOT_FOUND;
    }
    log_memory("before AFE creation");
    const size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (internal_largest < XIAOZHI_AFE_MIN_INTERNAL_BLOCK) {
        ESP_LOGE(TAG, "ESP-SR init failure: internal heap too small (largest=%u < %u); "
                      "WakeNet creation would abort and reboot the device",
                 (unsigned)internal_largest, (unsigned)XIAOZHI_AFE_MIN_INTERNAL_BLOCK);
        log_memory("AFE creation refused");
        /* Dump the size-class breakdown so the owner of the missing memory can
         * be identified from the log alone. */
        heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
        afe_config_free(config);
        return ESP_ERR_NO_MEM;
    }
    s_pipeline.afe = esp_afe_handle_from_config(config);
    if (s_pipeline.afe == NULL) {
        ESP_LOGE(TAG, "ESP-SR init failure: AFE interface unavailable");
        log_memory("ESP-SR AFE interface failure");
        afe_config_free(config);
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* Snapshot the tuning fields before the config is released: reading them
     * from the freed pointer afterwards is a use-after-free. */
    const int afe_mode = config->afe_mode;
    const int afe_core = config->afe_perferred_core;
    const int afe_priority = config->afe_perferred_priority;
    const int afe_ringbuf = config->afe_ringbuf_size;
    s_pipeline.afe_data = s_pipeline.afe->create_from_config(config);
    afe_config_free(config);
    if (s_pipeline.afe_data == NULL) {
        ESP_LOGE(TAG, "ESP-SR init failure: AFE instance unavailable");
        log_memory("ESP-SR AFE instance failure");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "AFE init: mode=%d core=%d priority=%d ringbuf=%d",
             afe_mode, afe_core, afe_priority, afe_ringbuf);
    log_memory("after AFE creation");
    return ESP_OK;
}

esp_err_t xiaozhi_audio_pipeline_prepare_encoder(void)
{
    /* The codec task is the only encoder owner. Retain this gate so startup
     * order remains stable while recording the pre-AFE memory budget. */
    log_memory("before Opus codec task");
    ESP_LOGI(TAG, "Opus encoder prepared: internal_free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

void xiaozhi_audio_pipeline_deinit(void)
{
    (void)i2s_audio_driver_set_output_muted(true);
    if (s_pipeline.codec_task != NULL) vTaskDeleteWithCaps(s_pipeline.codec_task);
    if (s_pipeline.detect_task != NULL) vTaskDeleteWithCaps(s_pipeline.detect_task);
    if (s_pipeline.feed_task != NULL) vTaskDeleteWithCaps(s_pipeline.feed_task);
    if (s_pipeline.afe != NULL && s_pipeline.afe_data != NULL) s_pipeline.afe->destroy(s_pipeline.afe_data);
    if (s_pipeline.encoder != NULL) esp_opus_enc_close(s_pipeline.encoder);
    if (s_pipeline.decoder != NULL) esp_opus_dec_close(s_pipeline.decoder);
    if (s_pipeline.pcm_frames != NULL) vQueueDeleteWithCaps(s_pipeline.pcm_frames);
    if (s_pipeline.controls != NULL) vQueueDelete(s_pipeline.controls);
    if (s_pipeline.tts_queue != NULL) vRingbufferDeleteWithCaps(s_pipeline.tts_queue);
    if (s_pipeline.playback_lock != NULL) vSemaphoreDelete(s_pipeline.playback_lock);
    heap_caps_free(s_pipeline.pcm_submit);
    heap_caps_free(s_pipeline.pcm_dropped);
    heap_caps_free(s_pipeline.codec_opus);
    heap_caps_free(s_pipeline.decode_pcm);
    heap_caps_free(s_pipeline.codec_frame);
    heap_caps_free(s_pipeline.preroll_buffer);
    memset(&s_pipeline, 0, sizeof(s_pipeline));
}

esp_err_t xiaozhi_audio_pipeline_init(xiaozhi_wake_word_cb_t wake_cb,
                                      xiaozhi_vad_state_cb_t vad_cb,
                                      xiaozhi_opus_frame_cb_t opus_cb,
                                      xiaozhi_tts_playback_cb_t playback_cb,
                                      xiaozhi_capture_complete_cb_t capture_complete_cb,
                                      void *context)
{
    if (s_pipeline.initialized) return ESP_OK;
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0U || !i2s_audio_driver_is_ready()) return ESP_ERR_INVALID_STATE;
    esp_err_t playout_err = audio_playback_service_init();
    if (playout_err != ESP_OK ||
        audio_playback_service_set_event_callback(playout_event, NULL) != ESP_OK) {
        return playout_err != ESP_OK ? playout_err : ESP_FAIL;
    }
    memset(&s_pipeline, 0, sizeof(s_pipeline));
    s_pipeline.wake_cb = wake_cb;
    s_pipeline.vad_cb = vad_cb;
    s_pipeline.opus_cb = opus_cb;
    s_pipeline.playback_cb = playback_cb;
    s_pipeline.capture_complete_cb = capture_complete_cb;
    s_pipeline.callback_context = context;
    s_pipeline.last_vad_state = VAD_SILENCE;
    s_pipeline.pcm_frames = xQueueCreateWithCaps(PCM_FRAME_QUEUE_DEPTH, sizeof(pcm_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pipeline.controls = xQueueCreate(AUDIO_CONTROL_QUEUE_DEPTH, sizeof(voice_audio_control_t));
    s_pipeline.tts_queue = xRingbufferCreateWithCaps(XIAOZHI_TTS_QUEUE_BYTES, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pipeline.playback_lock = xSemaphoreCreateMutex();
    s_pipeline.pcm_submit = heap_caps_malloc(sizeof(*s_pipeline.pcm_submit), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pipeline.pcm_dropped = heap_caps_malloc(sizeof(*s_pipeline.pcm_dropped), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pipeline.preroll_buffer = heap_caps_malloc(XIAOZHI_PREROLL_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_pipeline.pcm_frames == NULL || s_pipeline.controls == NULL || s_pipeline.tts_queue == NULL ||
        s_pipeline.playback_lock == NULL ||
        s_pipeline.pcm_submit == NULL || s_pipeline.pcm_dropped == NULL ||
        s_pipeline.preroll_buffer == NULL) goto failed;
    log_memory("after codec queues");
    if (xTaskCreatePinnedToCoreWithCaps(voice_codec_task, "voice_codec", VOICE_CODEC_STACK_BYTES, NULL, 4,
                                        &s_pipeline.codec_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) goto failed;
    for (unsigned wait = 0; wait < 50U && !s_pipeline.codec_ready && !s_pipeline.codec_failed; ++wait) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!s_pipeline.codec_ready) goto failed;
    if (init_afe() != ESP_OK) goto failed;
    if (xTaskCreatePinnedToCoreWithCaps(audio_feed_task, "voice_feed", 6144, NULL, 5,
                                        &s_pipeline.feed_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) goto failed;
    ESP_LOGI(TAG, "audio task created: voice_feed");
    if (xTaskCreatePinnedToCoreWithCaps(audio_detect_task, "voice_detect", 6144, NULL, 5,
                                        &s_pipeline.detect_task, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) goto failed;
    ESP_LOGI(TAG, "audio task created: voice_detect");
    s_pipeline.initialized = true;
    ESP_LOGI(TAG, "voice pipeline ready: WakeNet, VAD, single Opus codec (AEC/NS disabled)");
    return ESP_OK;
failed:
    ESP_LOGE(TAG, "voice pipeline setup failed");
    xiaozhi_audio_pipeline_deinit();
    return ESP_ERR_NO_MEM;
}

static esp_err_t post_control(voice_audio_control_t control)
{
    if (!s_pipeline.initialized || s_pipeline.controls == NULL) return ESP_ERR_INVALID_STATE;
    return xQueueSend(s_pipeline.controls, &control, pdMS_TO_TICKS(20)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t xiaozhi_audio_pipeline_set_streaming(bool enabled)
{
    return post_control(enabled ? VOICE_AUDIO_CONTROL_START_STREAM : VOICE_AUDIO_CONTROL_STOP_STREAM);
}

esp_err_t xiaozhi_audio_pipeline_queue_silence_opus_frames(uint32_t frame_count)
{
    if (!s_pipeline.initialized || !s_pipeline.codec_ready ||
        s_pipeline.opus_pcm_bytes == 0U || s_pipeline.opus_pcm_bytes > PCM_FRAME_MAX_BYTES) {
        return ESP_ERR_INVALID_STATE;
    }
    s_pipeline.silence_frames_epoch = s_pipeline.stream_epoch;
    s_pipeline.silence_frames_pending += frame_count;
    ESP_LOGI(TAG, "wake primer silence queued: frames=%u epoch=%u",
             (unsigned)frame_count, (unsigned)s_pipeline.stream_epoch);
    return ESP_OK;
}

void xiaozhi_audio_pipeline_reset_wake_word(void)
{
    (void)post_control(VOICE_AUDIO_CONTROL_STOP_STREAM);
}

void xiaozhi_audio_pipeline_arm_followup(void)
{
    (void)post_control(VOICE_AUDIO_CONTROL_ARM_FOLLOWUP);
}

void xiaozhi_audio_pipeline_reset_after_silence(void)
{
    (void)post_control(VOICE_AUDIO_CONTROL_RESET_AFTER_SILENCE);
}

void xiaozhi_audio_pipeline_reset_after_transport_failure(void)
{
    /* STOP_STREAM clears queued PCM and resets the AFE without disabling the
     * detector task; the explicit reset keeps the VAD baseline deterministic
     * when the control queue is serviced after a failed send. */
    (void)post_control(VOICE_AUDIO_CONTROL_STOP_STREAM);
    (void)post_control(VOICE_AUDIO_CONTROL_RESET_AFTER_SILENCE);
    ESP_LOGI(TAG, "voice transport failure cleanup queued: stream stopped, PCM cleared, AFE reset");
}

void xiaozhi_audio_pipeline_abort_playback(void)
{
    if (s_pipeline.playback_lock == NULL) return;
    xSemaphoreTake(s_pipeline.playback_lock, portMAX_DELAY);
    const uint32_t aborted_epoch = s_pipeline.playback_epoch;
    ++s_pipeline.playback_epoch;
    s_pipeline.playback_discard_pending = true;
    s_pipeline.tts_input_closed = false;
    s_pipeline.tts_finish_submitted = false;
    audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_TTS, aborted_epoch);
    xSemaphoreGive(s_pipeline.playback_lock);
}

void xiaozhi_audio_pipeline_finish_playback(void)
{
    if (s_pipeline.playback_lock == NULL) return;
    xSemaphoreTake(s_pipeline.playback_lock, portMAX_DELAY);
    s_pipeline.tts_input_closed = true;
    xSemaphoreGive(s_pipeline.playback_lock);
}

esp_err_t xiaozhi_audio_pipeline_begin_tts_playback(uint32_t *epoch)
{
    if (!s_pipeline.initialized || s_pipeline.playback_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_pipeline.playback_lock, portMAX_DELAY);
    /* 电台让位：TTS 要占用扬声器，电台会停掉拉流与解码、让出 HTTP socket、WiFi
       接收缓冲和 core 1 的解码时间。让位不等于终止，答完由语音服务调
       radio_stream_service_regain() 重新接回直播。 */
    (void)radio_stream_service_yield();
    discard_tts_queue();
    ++s_pipeline.playback_epoch;
    if (s_pipeline.playback_epoch == 0U) ++s_pipeline.playback_epoch;
    s_pipeline.playback_discard_pending = false;
    s_pipeline.tts_input_closed = false;
    s_pipeline.tts_finish_submitted = false;
    const uint32_t next_epoch = s_pipeline.playback_epoch;
    (void)post_control(VOICE_AUDIO_CONTROL_PAUSE_DETECT);
    const esp_err_t err = audio_playback_service_begin(AUDIO_PLAYOUT_SOURCE_TTS, next_epoch,
                                                       XIAOZHI_TTS_PREBUFFER_FRAMES);
    xSemaphoreGive(s_pipeline.playback_lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "TTS prebuffer ready frames=%u duration_ms=%u",
                 (unsigned)XIAOZHI_TTS_PREBUFFER_FRAMES,
                 (unsigned)(XIAOZHI_TTS_PREBUFFER_FRAMES * 30U));
    }
    if (err == ESP_OK && epoch != NULL) *epoch = next_epoch;
    return err;
}

void xiaozhi_audio_pipeline_resume_after_playback(void)
{
    (void)post_control(VOICE_AUDIO_CONTROL_RESUME_AFTER_PLAYBACK);
}

void xiaozhi_audio_pipeline_pause_detect(void)
{
    (void)post_control(VOICE_AUDIO_CONTROL_PAUSE_DETECT);
}

esp_err_t xiaozhi_audio_pipeline_enqueue_tts_opus(const uint8_t *opus, size_t opus_bytes)
{
    if (!s_pipeline.initialized || !s_pipeline.codec_ready) return ESP_ERR_INVALID_STATE;
    if (opus == NULL || opus_bytes == 0U || opus_bytes > XIAOZHI_TTS_PACKET_MAX_BYTES ||
        s_pipeline.playback_lock == NULL) return ESP_ERR_INVALID_ARG;
    const uint32_t enqueue_epoch = s_pipeline.playback_epoch;
    const size_t packet_bytes = sizeof(tts_packet_t) + opus_bytes;
    tts_packet_t *packet = NULL;
    if (xRingbufferSendAcquire(s_pipeline.tts_queue, (void **)&packet, packet_bytes, 0) == pdTRUE) {
        packet->epoch = enqueue_epoch;
        packet->length = opus_bytes;
        memcpy(packet->data, opus, opus_bytes);
    }
    if (packet == NULL) {
        ++s_pipeline.tts_queue_full_count;
        ESP_LOGW(TAG, "TTS queue full: count=%u bytes=%u",
                 (unsigned)s_pipeline.tts_queue_full_count, (unsigned)opus_bytes);
        return ESP_ERR_NO_MEM;
    }
    return xRingbufferSendComplete(s_pipeline.tts_queue, packet) == pdTRUE ? ESP_OK : ESP_FAIL;
}
