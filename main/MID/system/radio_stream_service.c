#include "radio_stream_service.h"

#include <stdbool.h>
#include <string.h>

#include "esp_audio_dec_default.h"
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
#include "i2s_audio_driver.h"

#define RADIO_PCM_BUFFER_BYTES 8192U
#define RADIO_CONV_BUFFER_BYTES 8192U
#define RADIO_INPUT_BUFFER_BYTES 16384U
/* HTTP 自己的接收缓冲与解码输入缓冲分开：前者在内部 RAM，保持小一点;
   后者需要容纳解析器探测帧边界所需的预填数据。 */
#define RADIO_HTTP_BUFFER_BYTES 4096U
/* 开播前先攒的帧数（每帧 836 B ≈ 26.1 ms @16 kHz 单声道，约 38 帧/秒）。
 *
 * 12 帧只有 ≈290 ms：实测日志 43440 开播时 queued=11，44020（才 580 ms）就出现
 * "startup PCM playout underrun: source=3"。和 TTS 的结论一样——服务端按实时
 * 供流、播放也按实时消耗，开播那一刻的水位就是整轮唯一的抖动缓冲，所以这个值
 * 只决定起手抗顿能力。提到 40 帧（≈1.04 s）。
 *
 * 它治不了中段断流：中段水位由网络供给决定，实测断流能到 3 s 以上（80860→83890
 * 供给为 0），队列从 130 帧耗到 14 帧。那段要靠放宽容许丢的 WiFi 接收缓冲，
 * 不是这里的值。 */
#define RADIO_PREBUFFER_FRAMES 40U
#define RADIO_URL "http://lhttp.qtfm.cn/live/20500038/64k.mp3"
#define RADIO_MAX_RECONNECTS 3U
/* 输入缓冲已满而解析器一口都不吃，连续这么多轮才判定真卡死。 */
#define RADIO_MAX_STALL_ROUNDS 8U
/* 回放队列满＝回放端跟不上直播，这是背压而不是电台故障。直播可以丢音，连续满
   这么多轮才说明回放端真的不消费了。 */
#define RADIO_MAX_QUEUE_FULL_ROUNDS 8U
/* 播放服务单次入队上限就是它（AUDIO_PLAYOUT_PCM_INPUT_MAX_BYTES）。 */
#define RADIO_ENQUEUE_CHUNK_BYTES 2048U

static const char *TAG = "radio_stream";
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static bool s_initialized;
static bool s_play_requested;
/* 让位（语音占用扬声器）与用户暂停走两条不同的路：
   - 让位＝把电台整段让出去：任务退出会话循环、关掉 HTTP 和解码器，等 regain()
     再重接直播。语音回合里电台因此不占 socket、不占 WiFi 接收缓冲、不占 core 1。
   - 用户暂停＝静音保活：继续拉流、继续解码，只把 PCM 丢掉，恢复时立刻出声。
   区分两者的第二重意义是解除权：regain() 只清 s_yielded，用户暂停必须等到
   “继续播放”。 */
static bool s_yielded;
static bool s_user_paused;
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

/* 播放服务的单次入队上限是 AUDIO_PLAYOUT_PCM_INPUT_MAX_BYTES(2048)，而 MP3 一帧
   解出来通常比它大得多，整帧丢过去会被 enqueue_pcm 直接判成 INVALID_ARG。 */
static esp_err_t enqueue_pcm_chunked(uint32_t epoch, const uint8_t *pcm, size_t bytes)
{
    size_t offset = 0U;
    while (offset < bytes) {
        const size_t remain = bytes - offset;
        const size_t chunk =
            remain > RADIO_ENQUEUE_CHUNK_BYTES ? RADIO_ENQUEUE_CHUNK_BYTES : remain;
        const esp_err_t err = audio_playback_service_enqueue_pcm(
            AUDIO_PLAYOUT_SOURCE_RADIO, epoch, pcm + offset, chunk);
        if (err != ESP_OK) {
            /* 真实错误码必须透传：INVALID_ARG 是通道已经易主（被语音抢了），
               TIMEOUT 才是队列真满了。原先一律塌成 ESP_FAIL，调用方分不出来，
               于是“被让位”也被当成电台故障。 */
            return err;
        }
        offset += chunk;
    }
    return ESP_OK;
}

/* 电台流的采样率由源端决定（蜻蜓FM 常见 44.1 kHz 立体声），而本地 I2S 固定
   LUMIDOCK_AUDIO_SAMPLE_RATE_HZ(16 kHz) 单声道。不转换就等于把 44.1 kHz 的数据
   按 16 kHz 发出去，声音会变成慢速低音。广播音质用线性插值已经够。 */
static size_t radio_to_16k_mono(const uint8_t *src, size_t src_bytes, uint32_t src_rate,
                                uint8_t channels, uint8_t bits, uint8_t *dst, size_t dst_cap)
{
    if (src == NULL || dst == NULL || bits != 16U || src_rate == 0U || channels == 0U)
        return 0U;
    const size_t frame_bytes = (size_t)channels * 2U;
    const size_t src_frames = src_bytes / frame_bytes;
    if (src_frames == 0U) return 0U;
    if (src_rate == LUMIDOCK_AUDIO_SAMPLE_RATE_HZ && channels == 1U) {
        const size_t copy = src_bytes < dst_cap ? src_bytes : dst_cap;
        memcpy(dst, src, copy);
        return copy;
    }
    const int16_t *in = (const int16_t *)src;
    int16_t *out = (int16_t *)dst;
    const size_t dst_cap_frames = dst_cap / 2U;
    size_t written = 0U;
    for (size_t i = 0U; i < dst_cap_frames; ++i) {
        const uint64_t pos = (uint64_t)i * src_rate;
        const size_t idx = (size_t)(pos / LUMIDOCK_AUDIO_SAMPLE_RATE_HZ);
        if (idx >= src_frames) break;
        const size_t next = idx + 1U < src_frames ? idx + 1U : idx;
        const int32_t frac = (int32_t)((pos % LUMIDOCK_AUDIO_SAMPLE_RATE_HZ) * 256ULL /
                                       LUMIDOCK_AUDIO_SAMPLE_RATE_HZ);
        const int32_t head = in[idx * channels];
        const int32_t tail = in[next * channels];
        int32_t value = head + (((tail - head) * frac) >> 8);
        if (channels >= 2U) {
            const int32_t head_r = in[idx * channels + 1U];
            const int32_t tail_r = in[next * channels + 1U];
            value = (value + (head_r + (((tail_r - head_r) * frac) >> 8))) / 2;
        }
        out[written++] = (int16_t)value;
    }
    return written * 2U;
}

static void radio_task(void *arg)
{
    (void)arg;
    uint8_t *input = heap_caps_malloc(RADIO_INPUT_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *pcm = heap_caps_malloc(RADIO_PCM_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* 解码输出是源采样率和源声道数，送进 I2S 前要转成 16 kHz 单声道，单独给块缓冲。 */
    uint8_t *conv = heap_caps_malloc(RADIO_CONV_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (input == NULL || pcm == NULL || conv == NULL) {
        ESP_LOGE(TAG, "MP3 buffers allocation failed");
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool requested = s_play_requested;
        const bool yield_parked = s_yielded;
        uint32_t epoch = s_epoch;
        xSemaphoreGive(s_lock);
        /* 让位期间不建立也不保持任何流：这正是要让出去的那部分资源。等 regain()
           清掉 s_yielded 再重新接回直播（直播可以丢音，直接接当前时刻）。 */
        if (!requested || yield_parked) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }

        ESP_LOGI(TAG, "radio state=CONNECTING epoch=%u", (unsigned)epoch);
        esp_http_client_config_t cfg = {.url = RADIO_URL, .timeout_ms = 12000, .buffer_size = RADIO_HTTP_BUFFER_BYTES};
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
        /* MP3 is a bare decoder, so two registries are involved:
         * esp_audio_simple_dec_register_default() only registers the container
         * parsers (WAV/M4A/TS/OGG).  The decoder registry the simple decoder
         * looks MP3 up in is filled by esp_audio_dec_register_default(), and
         * without that call the stream opens and then dies with "Decoder MP3 not
         * registered" (AUDIO_DEC), leaving the radio silent.  Both are one-off
         * global registrations, so they are guarded instead of being repeated on
         * every reconnect. */
        static bool decoders_registered = false;
        if (!decoders_registered) {
            (void)esp_audio_dec_register_default();
            (void)esp_audio_simple_dec_register_default();
            decoders_registered = true;
        }
        esp_audio_simple_dec_cfg_t dec_cfg = {.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3, .use_frame_dec = false};
        esp_audio_simple_dec_handle_t decoder = NULL;
        const esp_audio_err_t dec_open_err = esp_audio_simple_dec_open(&dec_cfg, &decoder);
        if (dec_open_err != ESP_AUDIO_ERR_OK) {
            record_failure("decoder_open", dec_open_err, 0);
            esp_http_client_close(http); esp_http_client_cleanup(http);
            xSemaphoreTake(s_lock, portMAX_DELAY); s_play_requested = false; xSemaphoreGive(s_lock);
            audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_RADIO, epoch);
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool start_muted = s_yielded || s_user_paused;
        xSemaphoreGive(s_lock);
        /* 让位/暂停期间不能抢扬声器：抢了会把语音正在播的 TTS 冲掉。 */
        bool armed = false;
        if (!start_muted) {
            if (audio_playback_service_begin(AUDIO_PLAYOUT_SOURCE_RADIO, epoch, RADIO_PREBUFFER_FRAMES) != ESP_OK) {
                record_failure("playback_begin", ESP_FAIL, 0);
                esp_audio_simple_dec_close(decoder); esp_http_client_close(http); esp_http_client_cleanup(http);
                xSemaphoreTake(s_lock, portMAX_DELAY); s_play_requested = false; xSemaphoreGive(s_lock);
                continue;
            }
            armed = true;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY); s_active = true; xSemaphoreGive(s_lock);
        size_t input_len = 0U;
        uint8_t decode_failures = 0U;
        uint8_t stall_rounds = 0U;
        uint8_t queue_full_rounds = 0U;
        uint32_t stream_rate = 0U;
        uint8_t stream_channels = 0U;
        bool retry = false;
        bool parked = false;
        while (true) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            /* 用户暂停仍是静音保活：不打断会话循环，否则“继续播放”得重连。 */
            const bool keep = s_play_requested && s_epoch == epoch;
            const bool yielded = s_yielded;
            const bool muted = yielded || s_user_paused;
            const bool paused_by_user = s_user_paused;
            xSemaphoreGive(s_lock);
            if (!keep) break;
            /* 让位＝把电台整段让出去：不再往下拉流解码，退出会话循环，把 HTTP
               socket、WiFi 接收缓冲和 core 1 上的 MP3 解码全部让给语音链路。
               regain() 解除后由外层循环重新接回直播流。 */
            if (yielded) { parked = true; break; }
            if (input_len == RADIO_INPUT_BUFFER_BYTES) {
                /* 缓冲满了说明解析器一口都没吃。它是在等更多数据来确定帧边界，
                   而不是流本身断了；广播流是无界的，把已攒的数据丢掉让它从下一个
                   帧同步点重新对齐即可——直播可以丢音，但不能停在这里不出声。
                   原实现是直接 break，于是电台一哑到底且不留任何失败日志。 */
                if (++stall_rounds >= RADIO_MAX_STALL_ROUNDS) {
                    record_failure("parser_stall", ESP_ERR_INVALID_STATE, s_reconnects);
                    break;
                }
                ESP_LOGW(TAG, "MP3 input buffer full with no progress; dropped %u bytes to resync (%u/%u)",
                         (unsigned)input_len, (unsigned)stall_rounds, (unsigned)RADIO_MAX_STALL_ROUNDS);
                input_len = 0U;
                continue;
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
                stall_rounds = 0U;
                esp_audio_simple_dec_info_t info = {0};
                /* 格式信息要等解码器吐出第一帧才就绪；拿不到就按它给的原始 PCM
                   直接送，至少不要因为查不到格式把电台断掉。 */
                const bool have_info =
                    esp_audio_simple_dec_get_info(decoder, &info) == ESP_AUDIO_ERR_OK &&
                    info.sample_rate != 0U && info.bits_per_sample != 0U;
                if (have_info &&
                    (info.sample_rate != stream_rate || info.channel != stream_channels)) {
                    ESP_LOGI(TAG, "radio stream format: %u Hz %u ch %u bit",
                             (unsigned)info.sample_rate, (unsigned)info.channel,
                             (unsigned)info.bits_per_sample);
                    stream_rate = info.sample_rate;
                    stream_channels = info.channel;
                }
                const size_t pcm_bytes =
                    have_info ? radio_to_16k_mono(pcm, out.decoded_size, info.sample_rate,
                                                  info.channel, info.bits_per_sample, conv,
                                                  RADIO_CONV_BUFFER_BYTES)
                              : out.decoded_size;
                const uint8_t *payload = have_info ? conv : pcm;
                if (pcm_bytes == 0U) {
                    /* decode_failures 只在真正送出去之后才清零。否则“解出了数据但
                       转换不出 16 kHz”会被下一帧的成功重置，阈值永远触发不了，
                       格式异常时就又变成没声音、也没日志。 */
                    if (++decode_failures >= 3U) {
                        record_failure("pcm_convert", ESP_FAIL, s_reconnects);
                        break;
                    }
                } else if (muted) {
                    /* 只有用户暂停会走到这里（让位已经在循环开头退出了）。直播可以
                       丢音：静音期间照常拉流解码，只把 PCM 扔掉、不补播，解除静音
                       时直接接上当前时刻。 */
                    if (armed) {
                        audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_RADIO, epoch);
                        armed = false;
                        ESP_LOGI(TAG, "电台静音保活：丢弃 PCM paused=%d", (int)paused_by_user);
                    }
                    decode_failures = 0U;
                } else {
                    if (!armed) {
                        /* 抢回扬声器：让位解除后第一次出声。 */
                        if (audio_playback_service_begin(AUDIO_PLAYOUT_SOURCE_RADIO, epoch,
                                                         RADIO_PREBUFFER_FRAMES) != ESP_OK) {
                            ESP_LOGW(TAG, "电台抢回扬声器失败 epoch=%u", (unsigned)epoch);
                            break;
                        }
                        armed = true;
                        ESP_LOGI(TAG, "电台抢回扬声器 epoch=%u", (unsigned)epoch);
                    }
                    const esp_err_t enq = enqueue_pcm_chunked(epoch, payload, pcm_bytes);
                    if (enq == ESP_ERR_INVALID_ARG) {
                        /* 通道已经易主＝被语音抢占，这是让位而不是故障：转入静音
                           保活等 regain()，绝不记 record_failure，更不能退出电台。 */
                        xSemaphoreTake(s_lock, portMAX_DELAY);
                        s_yielded = true;
                        xSemaphoreGive(s_lock);
                        armed = false;
                    } else if (enq == ESP_ERR_TIMEOUT) {
                        /* 队列满＝回放端跟不上直播，这是背压而不是电台故障。直播
                           可以丢音：丢掉这一帧继续接当前时刻。原实现把 TIMEOUT
                           和真错误合并处理，于是一次积压就把整个电台会话拆掉重连，
                           用户听到的就是断流。 */
                        if (++queue_full_rounds >= RADIO_MAX_QUEUE_FULL_ROUNDS) {
                            record_failure("pcm_enqueue", enq, s_reconnects);
                            break;
                        }
                        ESP_LOGW(TAG, "电台回放积压：丢弃 %u 字节直播帧（%u/%u）",
                                 (unsigned)pcm_bytes, (unsigned)queue_full_rounds,
                                 (unsigned)RADIO_MAX_QUEUE_FULL_ROUNDS);
                    } else if (enq != ESP_OK) {
                        record_failure("pcm_enqueue", enq, s_reconnects);
                        break;
                    } else {
                        queue_full_rounds = 0U;
                        decode_failures = 0U;
                    }
                }
            } else if (dec_err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                ESP_LOGW(TAG, "MP3 decode output buffer too small: needed=%u have=%u",
                         (unsigned)out.needed_size, (unsigned)RADIO_PCM_BUFFER_BYTES);
                if (++decode_failures >= 3U) {
                    record_failure("pcm_buffer", ESP_ERR_NO_MEM, s_reconnects);
                    break;
                }
            } else if (dec_err != ESP_AUDIO_ERR_OK) {
                if (++decode_failures >= 3U) {
                    record_failure("mp3_decode", dec_err, s_reconnects);
                    break;
                }
            } else {
                /* decoded_size == 0：解析器吃掉了数据但还没凑够一帧，是正常的中间
                   态，不能算解码失败——否则流开头几次空返回就会把整条流判死。 */
                decode_failures = 0U;
                stall_rounds = 0U;
            }
            if (raw.consumed > 0U) stall_rounds = 0U;
            if (raw.consumed > 0U && raw.consumed < input_len) memmove(input, input + raw.consumed, input_len - raw.consumed);
            input_len = raw.consumed <= input_len ? input_len - raw.consumed : 0U;
        }
        audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_RADIO, epoch);
        esp_audio_simple_dec_close(decoder);
        esp_http_client_close(http); esp_http_client_cleanup(http);
        xSemaphoreTake(s_lock, portMAX_DELAY); s_active = false; xSemaphoreGive(s_lock);
        if (parked) {
            /* 让位不是结束：s_play_requested 必须保持为真，否则电台不会自己回来。
               epoch 换一个，免得重新接流时旧播放通道还认这个号。 */
            xSemaphoreTake(s_lock, portMAX_DELAY);
            ++s_epoch;
            if (s_epoch == 0U) s_epoch = 1U;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "电台让位：已停拉流与解码，等 regain 后重接直播");
        } else if (retry) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_play_requested && s_epoch == epoch) { ++s_epoch; if (s_epoch == 0U) s_epoch = 1U; }
            xSemaphoreGive(s_lock);
            vTaskDelay(pdMS_TO_TICKS(250));
        } else {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            /* 走到这里说明是真正的失败，或是用户 stop()。让位走的是上面的 parked
               分支，不会清播放请求，所以这里可以无条件清掉：否则流已经断了，外层
               循环还会一轮轮重连下去。 */
            s_play_requested = false;
            xSemaphoreGive(s_lock);
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
    xSemaphoreTake(s_lock, portMAX_DELAY); ++s_epoch; if (s_epoch == 0U) s_epoch = 1U; s_play_requested = true; s_user_paused = false; s_reconnects = 0U; xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    ESP_LOGI(TAG, "开始播放固定 MP3 电台");
    return ESP_OK;
}

/* 暂停是静音保活而不是断流：会话循环继续把 HTTP 流读下去，只是不出声，于是
   “继续播放”不用重连、不会重新缓冲。真正终止电台的只有 stop()。 */
esp_err_t radio_stream_service_pause(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_user_paused = true;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    ESP_LOGI(TAG, "电台暂停（静音保活）");
    return ESP_OK;
}

/* 只解除“用户暂停”。s_yielded 归语音侧管：一句话还没答完时，“继续播放”若
   顺手把让位也解了，电台会先响一下、又被紧接着的 TTS 让位掉，听上去就是卡了两下。
   保持让位，等这一轮答完 regain() 自然出声。 */
esp_err_t radio_stream_service_resume(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_user_paused = false;
    s_play_requested = true;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    ESP_LOGI(TAG, "电台恢复播放");
    return ESP_OK;
}

esp_err_t radio_stream_service_stop(void) { if (!s_initialized) return ESP_ERR_INVALID_STATE; xSemaphoreTake(s_lock, portMAX_DELAY); s_play_requested = false; s_yielded = false; s_user_paused = false; ++s_epoch; xSemaphoreGive(s_lock); xTaskNotifyGive(s_task); return ESP_OK; }

/* 让位：语音要占用扬声器。置位后电台任务会退出会话循环、关掉 HTTP 与解码器，
   把 socket、WiFi 接收缓冲和 core 1 的解码时间让给语音链路；regain() 解除后重新
   接回直播流。让位不等于终止：s_play_requested 保持为真。 */
esp_err_t radio_stream_service_yield(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool was_yielded = s_yielded;
    s_yielded = true;
    xSemaphoreGive(s_lock);
    if (!was_yielded) ESP_LOGI(TAG, "电台让位：语音占用扬声器");
    return ESP_OK;
}

/* 只清让位，不动 s_user_paused：用户暂停必须由“继续播放”解除。让位期间任务停在
   外层待机循环里，notify 一下立刻回去重连，不用等 100 ms 轮询周期。 */
esp_err_t radio_stream_service_regain(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool was_yielded = s_yielded;
    s_yielded = false;
    xSemaphoreGive(s_lock);
    if (was_yielded) {
        ESP_LOGI(TAG, "电台让位解除：准备抢回扬声器");
        xTaskNotifyGive(s_task);
    }
    return ESP_OK;
}

bool radio_stream_service_is_active(void) { return s_active && !s_user_paused; }

bool radio_stream_service_is_requested(void)
{
    if (!s_initialized) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool requested = s_play_requested && !s_user_paused;
    xSemaphoreGive(s_lock);
    return requested;
}

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
