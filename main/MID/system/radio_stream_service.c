#include "radio_stream_service.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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

/* 电台源。两种取流方式共用下面同一套解码/转换/入队/背压循环，差别只在“字节从哪来”：
 *   MP3 —— 一条长连接，读到的字节就是 MP3 帧。蜻蜓FM 的静态直播流是这种。
 *   HLS —— 先读 .m3u8 播放列表，再一片片顺序拉 .ts 分片。央广网等官方源都是这种。
 * 蜻蜓FM 已经下架了中国之声的静态 MP3 源（live/386 实测 404，而同域名的 20500038
 * 仍正常吐流，所以是单个 ID 下架而不是域名失效）。要听“正规”电台就只剩 HLS 一条路。
 * 切回旧的只要改 RADIO_SOURCE 这一个宏。 */
#define RADIO_URL_MP3 "http://lhttp.qtfm.cn/live/20500038/64k.mp3"
#define RADIO_URL_HLS "http://ngcdn001.cnr.cn/live/zgzs/index.m3u8"

typedef enum {
    RADIO_SOURCE_MP3 = 0,
    RADIO_SOURCE_HLS,
} radio_source_kind_t;
#define RADIO_SOURCE RADIO_SOURCE_HLS

/* HLS 参数。CNR 实测：播放列表里保留 6 个分片、每个 10 s（TARGETDURATION=10），
   窗口整体滑动；片名是连续整数 + ".ts"；无鉴权；纯 HTTP（不用碰 TLS）。 */
/* 从窗口里取倒数第二片开始播：落后直播约 20 s，换来同量级的抖动余量。
   取最后一片只剩 10 s 余量；取最旧一片虽然余量最大，但落后 60 s 太夸张。
   取片和直播都以实时速度推进，所以这个落后量不会越拉越大；真掉出滑动窗口时
   分片会 404，那时自动重拉列表重新对齐。 */
#define RADIO_HLS_PLAYLIST_MAX 1024U
#define RADIO_HLS_URL_MAX 224U
/* 一次 hls_read 里最多连续换这么多片。超过就说明分片拿得到却读不出数据
   （循环里每轮都要发一次 HTTP 请求），必须交给外层重连，不能在这里空转。 */
#define RADIO_HLS_MAX_ADVANCE_PER_READ 4U

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

/* ------------------------------------------------------------------ HLS 取流 */
typedef struct {
    esp_http_client_handle_t http; /* 整段会话复用同一个 handle */
    char base[RADIO_HLS_URL_MAX];  /* 分片目录，含结尾的 '/' */
    uint64_t next_index;           /* 下一个要拉的分片序号 */
    bool segment_open;             /* 当前分片的响应还没读完 */
} radio_hls_t;

/* 读一次播放列表，解析出分片目录和起始序号。列表约 200 字节，一次读完。
 *
 * CNR 实测格式：
 *   #EXTM3U / #EXT-X-VERSION:3 / #EXT-X-MEDIA-SEQUENCE:15907434
 *   #EXT-X-TARGETDURATION:10 / #EXTINF:10.006, / 15907434.ts / ...
 * 片名是连续整数，所以记住目录 + 序号之后直接加号就能推下一片，不必每片都重拉列表。
 *
 * reopen=false 用在刚连上时：那时的响应就是列表本身，直接用。
 * reopen=true 用在掉出滑动窗口后的重新对齐：列表和正在读的分片不是一条流，
 * 必须先 close() 才能开新的（不分片必须重开连接，这种重同步很少发生）。 */
static bool hls_read_playlist(radio_hls_t *hls, bool reopen)
{
    if (reopen) {
        esp_http_client_set_url(hls->http, RADIO_URL_HLS);
        esp_http_client_close(hls->http);
        if (esp_http_client_open(hls->http, 0) != ESP_OK) return false;
        (void)esp_http_client_fetch_headers(hls->http);
    }
    char body[RADIO_HLS_PLAYLIST_MAX];
    size_t used = 0U;
    while (used < sizeof(body) - 1U) {
        const int n = esp_http_client_read(hls->http, body + used,
                                           (int)(sizeof(body) - 1U - used));
        if (n <= 0) break;
        used += (size_t)n;
    }
    body[used] = '\0';

    /* 分片目录 = 播放列表 URL 去掉最后一个路径段。 */
    strncpy(hls->base, RADIO_URL_HLS, sizeof(hls->base) - 1U);
    hls->base[sizeof(hls->base) - 1U] = '\0';
    char *slash = strrchr(hls->base, '/');
    if (slash == NULL) return false;
    slash[1] = '\0';

    /* 扫出最后两个分片行，取倒数第二片。 */
    uint64_t last_index = 0U;
    uint64_t prev_index = 0U;
    char *cursor = body;
    while (*cursor != '\0') {
        char *nl = strchr(cursor, '\n');
        if (nl != NULL) *nl = '\0';
        const size_t len = strlen(cursor);
        if (len > 0U && cursor[0] != '#') {
            const uint64_t index = strtoull(cursor, NULL, 10);
            /* 非数字片名推不出下一片，本实现不支持；跳过而不是当成序号 0。 */
            if (index != 0U) {
                prev_index = last_index;
                last_index = index;
            }
        }
        if (nl == NULL) break;
        cursor = nl + 1U;
    }
    if (last_index == 0U) return false;
    hls->next_index = prev_index != 0U ? prev_index : last_index;
    hls->segment_open = false;
    ESP_LOGI(TAG, "HLS 列表就绪：窗口 %llu..%llu，从 %llu 开始",
             (unsigned long long)prev_index, (unsigned long long)last_index,
             (unsigned long long)hls->next_index);
    return true;
}

static bool hls_open_segment(radio_hls_t *hls, uint64_t index)
{
    char url[RADIO_HLS_URL_MAX];
    const int written = snprintf(url, sizeof(url), "%s%llu.ts", hls->base,
                                 (unsigned long long)index);
    if (written <= 0 || (size_t)written >= sizeof(url)) return false;

    /* 每个分片都开一条新连接，不复用上一片的 socket。
     *
     * 原实现想省掉这次握手（IDF 的 esp_http_client_connect 只在
     * state < HTTP_STATE_CONNECTED 时才真重连），但实机上换来的是更糟的结果：分片
     * 服务器常在响应完就关连接，而 state 已到 CONNECTED 的 open() 会跳过重连、把
     * 请求写进一条死 socket，随后整段卡在 timeout_ms 上——实测电台一次哑掉 11.7 秒
     * （日志 49990 → 61710 没有任何 PCM 入队，队列从 46 帧耗干后 underrun）。
     * 代价那边：实测建连只要约 240 ms，而分片是 10 秒一个，握手开销被 1 秒的播放
     * 预缓冲完全吸收。别再改回复用。 */
    esp_http_client_set_url(hls->http, url);
    esp_http_client_close(hls->http);
    if (esp_http_client_open(hls->http, 0) != ESP_OK) return false;
    (void)esp_http_client_fetch_headers(hls->http);
    hls->segment_open = true;
    return true;
}

/* HLS 的字节源：把当前分片的响应体当成一个会结束的流，读完了就换下一片。
   返回 >0 为拿到的字节数；返回 0 表示这一轮彻底没进展，交给外层按网络停顿处理。 */
static int hls_read(radio_hls_t *hls, uint8_t *buf, size_t len)
{
    for (uint8_t attempt = 0U; attempt < RADIO_HLS_MAX_ADVANCE_PER_READ; ++attempt) {
        if (!hls->segment_open) {
            if (!hls_open_segment(hls, hls->next_index)) {
                /* 拉不到多半是已经落后到滑动窗口之外了。重拉列表拿新序号。 */
                ESP_LOGW(TAG, "HLS 分片 %llu 拉不到；重拉播放列表重新对齐",
                         (unsigned long long)hls->next_index);
                if (!hls_read_playlist(hls, true)) return 0;
                if (!hls_open_segment(hls, hls->next_index)) return 0;
            }
        }
        const int n = esp_http_client_read(hls->http, (char *)buf, (int)len);
        if (n > 0) return n;
        /* 这一片读完（或读出错）：收流，下一轮换下一片。读出错也走这条路——
           下一片的 open() 会失败，进而触发上面那次列表重拉。 */
        hls->segment_open = false;
        ++hls->next_index;
    }
    ESP_LOGW(TAG, "HLS 连续 %u 片都没读出数据；按网络停顿处理",
             (unsigned)RADIO_HLS_MAX_ADVANCE_PER_READ);
    return 0;
}

static void radio_task(void *arg)
{
    (void)arg;
    uint8_t *input = heap_caps_malloc(RADIO_INPUT_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *pcm = heap_caps_malloc(RADIO_PCM_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* 解码输出是源采样率和源声道数，送进 I2S 前要转成 16 kHz 单声道，单独给块缓冲。 */
    uint8_t *conv = heap_caps_malloc(RADIO_CONV_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (input == NULL || pcm == NULL || conv == NULL) {
        ESP_LOGE(TAG, "radio buffers allocation failed");
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
        radio_hls_t hls = {0};
        esp_http_client_config_t cfg = {
            .url = RADIO_SOURCE == RADIO_SOURCE_HLS ? RADIO_URL_HLS : RADIO_URL_MP3,
            /* 超时从一个坏 socket 里拔出来要等多久。实测连 CNR 建连只要约 240 ms，
               所以 4 s 已经有十几倍余量；而设成 12 s 的代价是电台一次哑 11.7 秒。 */
            .timeout_ms = 4000,
            .buffer_size = RADIO_HTTP_BUFFER_BYTES,
        };
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
        /* HLS：刚打开的就是播放列表本身，直接在这个响应上解析，不用再连一次。 */
        if (RADIO_SOURCE == RADIO_SOURCE_HLS) {
            hls.http = http;
            if (!hls_read_playlist(&hls, false)) {
                record_failure("hls_playlist", ESP_ERR_INVALID_RESPONSE, 0);
                esp_http_client_close(http); esp_http_client_cleanup(http);
                xSemaphoreTake(s_lock, portMAX_DELAY); s_play_requested = false; xSemaphoreGive(s_lock);
                audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_RADIO, epoch);
                continue;
            }
        }
        /* The container parser is what varies by source: MP3 is a bare decoder, while
         * HLS feeds TS packets.  Either way two registries are involved:
         * esp_audio_simple_dec_register_default() only registers the container
         * parsers (WAV/M4A/TS/OGG).  The decoder registry the simple decoder
         * looks the codec up in is filled by esp_audio_dec_register_default(), and
         * without that call the stream opens and then dies with "Decoder ... not
         * registered" (AUDIO_DEC), leaving the radio silent.  Both are one-off
         * global registrations, so they are guarded instead of being repeated on
         * every reconnect. */
        static bool decoders_registered = false;
        if (!decoders_registered) {
            (void)esp_audio_dec_register_default();
            (void)esp_audio_simple_dec_register_default();
            decoders_registered = true;
        }
        /* TS 里的音频是 AAC。解码器注册表由 esp_audio_dec_register_default() 填，
           前提是 menuconfig 里 CONFIG_AUDIO_DECODER_AAC_SUPPORT 为 y（当前为 y，
           并已写进 sdkconfig.defaults 防重新生成时静默回退）。aac_plus_enable 一起
           打开：分片里是 HE-AAC 时不开会解不出声。 */
        esp_ts_dec_cfg_t ts_cfg = {.aac_plus_enable = true};
        esp_audio_simple_dec_cfg_t dec_cfg = {
            .dec_type = RADIO_SOURCE == RADIO_SOURCE_HLS ? ESP_AUDIO_SIMPLE_DEC_TYPE_TS
                                                         : ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
            .dec_cfg = RADIO_SOURCE == RADIO_SOURCE_HLS ? &ts_cfg : NULL,
            .cfg_size = RADIO_SOURCE == RADIO_SOURCE_HLS ? sizeof(ts_cfg) : 0U,
            .use_frame_dec = false,
        };
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
        /* 上一轮解析器实际消化的字节数，决定这一轮是接着喂还是去读。 */
        size_t last_consumed = 0U;
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
                ESP_LOGW(TAG, "radio input buffer full with no progress; dropped %u bytes to resync (%u/%u)",
                         (unsigned)input_len, (unsigned)stall_rounds, (unsigned)RADIO_MAX_STALL_ROUNDS);
                input_len = 0U;
                last_consumed = 0U;
                continue;
            }
            /* 上一轮解析器还消化了字节，就接着把它喂完，这一轮不去读新的。
             *
             * esp_audio_simple_dec_process 一次只消化能填满输出缓冲的那几帧（输出
             * 8 KB，而 48 kHz 立体声 AAC 一帧就是 1024×2 ch×2 B = 4 KB，所以一次
             * 约两帧、几百字节），而 HLS 的分片响应体在 socket 里已经就绪，一次
             * esp_http_client_read 能把整个 16 KB 输入缓冲拉满。只喂一次就回头再读的话，
             * 读入是消化的三十几倍，输入缓冲必然一直满、一直整块丢掉——实机就是响一两秒
             * 之后再也没有声音，而且这个循环会持续烧 core 1 和 PSRAM 带宽。旧的 MP3 长
             * 连接没暴露它，只因为服务端按实时供流，一次 read 通常只有一两 KB。 */
            if (input_len == 0U || last_consumed == 0U) {
                /* 唯一的“字节从哪来”分歧点：MP3 是长连接直读，HLS 是分片响应体、读完
                   一片自动换下一片。往下的解码/重采样/入队/背压处理完全共用。 */
                int n = RADIO_SOURCE == RADIO_SOURCE_HLS
                            ? hls_read(&hls, input + input_len, RADIO_INPUT_BUFFER_BYTES - input_len)
                            : esp_http_client_read(http, (char *)input + input_len,
                                                   (int)(RADIO_INPUT_BUFFER_BYTES - input_len));
                if (n == 0) {
                    if (s_reconnects < RADIO_MAX_RECONNECTS) { ++s_reconnects; retry = true; ESP_LOGW(TAG, "radio network stall; reconnect=%u/%u", (unsigned)s_reconnects, (unsigned)RADIO_MAX_RECONNECTS); }
                    else record_failure("http_read_eof", 0, s_reconnects);
                    break;
                }
                if (n < 0) { record_failure("http_read", n, s_reconnects); break; }
                input_len += (size_t)n;
            }
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
                ESP_LOGW(TAG, "decode output buffer too small: needed=%u have=%u",
                         (unsigned)out.needed_size, (unsigned)RADIO_PCM_BUFFER_BYTES);
                if (++decode_failures >= 3U) {
                    record_failure("pcm_buffer", ESP_ERR_NO_MEM, s_reconnects);
                    break;
                }
            } else if (dec_err != ESP_AUDIO_ERR_OK) {
                if (++decode_failures >= 3U) {
                    record_failure("radio_decode", dec_err, s_reconnects);
                    break;
                }
            } else {
                /* decoded_size == 0：解析器吃掉了数据但还没凑够一帧，是正常的中间
                   态，不能算解码失败——否则流开头几次空返回就会把整条流判死。 */
                decode_failures = 0U;
                stall_rounds = 0U;
            }
            last_consumed = raw.consumed;
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
