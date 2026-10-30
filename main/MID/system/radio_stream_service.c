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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio_playback_service.h"
#include "device_data.h"
#include "i2s_audio_driver.h"

#define RADIO_PCM_BUFFER_BYTES 8192U
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

/* 拉流限速：HLS 分片是一整片下载的，客户端会按链路能跑多快就拉多快，而电台内容
 * 本身只有 64~128 kbps。全速拉的时候 WiFi 接收路径要在很短的时间里接住上百 KB，
 * 逼着驱动去要一大批网络缓冲（eb）——而 eb 只能从内部 DMA 内存里拿，偏偏那时内部
 * RAM 已经被 BLE 配网栈和 WSS 挤到只剩几 KB（实测电台在放 + WSS 建连后
 * internal=3615 largest=2048），驱动拿不到就打 "wifi: m f null" 丢包，丢包就是
 * TCP 收窗停滞，最后表现成电台一哑十几秒（实测 48687→58237 之间有 9.5 秒没有
 * 任何 PCM 入队，队列从 103 帧被抽干到 0）。
 *
 * 按 24 KB/s 摊平之后，同时压在驱动身上的包少了一个数量级，eb 就不再是瓶颈。
 * 对内容没有影响：分片 10 秒一个，128 kbps 也只有 16 KB/s，摊平后 7 秒左右拉完，
 * 开播那 1 秒预缓冲完全吸收得了，队列还有 4 秒多的余量扛抖动。
 *
 * 但上面那条推算带一个没验证过的前提：“摊平之后一定追得过内容码率”。实测把这个
 * 前提推翻了一半——39219 开播时 queued=39，到 44249 就只剩 3 帧，此后整段播放都
 * 停在 0~4 帧（每帧≈21.3 ms，也就是只剩 0~90 ms 的抖动余量）。说明拉流速率仍然
 * 只跟播放速率持平，一点缺口都补不回来：每 10 秒一次的换片建连（≈240 ms）都会漏
 * 成静音，网络再抖一下就更明显。
 *
 * 所以限速不再是一个固定上限，而是跟着播放队列水位走。限速的本意始终是“别把整片
 * 一次性拉完冲垮 WiFi 接收缓冲”，而不是“缺口已经出来了也不许追”——单次读仍然
 * 只有 2 KB，追赶也只是把读取的间隔压短，瞬时压在驱动身上的包并没有变多。 */
#define RADIO_HLS_TARGET_BYTES_PER_SEC 24576U
#define RADIO_HLS_CATCHUP_BYTES_PER_SEC 98304U
#define RADIO_HLS_THROTTLE_WINDOW_US 200000
/* 水位门槛（帧）。电台每帧 ≈21.3 ms：40 帧≈0.85 s 是开播预缓冲那一档，低于它就
   说明垫子已经见底，该追了；160 帧≈3.4 s 说明回放端在积压，回到 24 KB/s 摊平让
   队列自然回落，不至于顶到 256 帧上限触发 "PCM playout queue full" 丢帧。 */
#define RADIO_HLS_QUEUE_LOW_FRAMES 40U
#define RADIO_HLS_QUEUE_HIGH_FRAMES 160U

/* 电台频道表。全部走 HLS。
 *
 * 蜻蜓FM 的 MP3 长连接路径已经删掉：它的取流方式、错误处理和这套 HLS 完全不同，
 * 两条并存要维护两套；蜻蜓那版留在 git 标签 qtfm-radio-checkpoint 上，真要用再切回。
 *
 * 央广的源规则很整齐：http://ngcdn001.cnr.cn/live/{代号}/index.m3u8。代号是拼音首
 * 字母，但**不能凭猜**——华夏之声的 hzzs 实测就是 404。加台前先拿浏览器或 curl 打
 * 一下这个地址，能看到 #EXTM3U 才算数；拿不到的分片会让整个台一哑到底。
 *
 * 频道只顺着下标走：用户说"换一个台"就取下一个，谁也不用报得出台名。名字留着是
 * 给日志和设备状态用的——真去匹配名字时，云端 ASR 会把"音乐之声"切成"音乐之声儿"，
 * 匹配的容错最后都堆在设备这边，越堆越像猜。 */
typedef struct {
    const char *name; /* 日志和设备状态用，不参与选台 */
    const char *url;
} radio_station_t;

#define RADIO_STATION_COUNT 4U
static const radio_station_t k_stations[RADIO_STATION_COUNT] = {
    {"中国之声", "http://ngcdn001.cnr.cn/live/zgzs/index.m3u8"},
    {"经济之声", "http://ngcdn001.cnr.cn/live/jjzs/index.m3u8"},
    {"音乐之声", "http://ngcdn001.cnr.cn/live/yyzs/index.m3u8"},
    {"文艺之声", "http://ngcdn001.cnr.cn/live/wyzs/index.m3u8"},
};
/* 只说“打开电台”没点名字时放这个。 */
#define RADIO_STATION_DEFAULT 0U

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
/* 单次 read 的字节上限。
 *
 * 原来这里直接用输入缓冲的剩余空间（16 KB）当上限，一次就拉 16 KB。旧 MP3 长连接
 * 没这个问题，因为服务端按实时供流、一次 read 通常只有一两 KB（见下面循环里的注释）。
 * HLS 的分片响应体在 socket 里是现成的，一次 16 KB 会把 WiFi 接收缓冲瞬间占满，
 * WSS 的包只能排在后面——实测电台播放中 PONG 从正常的 10 秒拖到 21~40 秒，云端对
 * 唤醒的回应几十秒上不来。
 *
 * 限到 2 KB 是回到旧路径的读取节奏：总量不变，但摊到更多轮里，每轮之间都留给 WSS
 * 一次机会。代价是多几次 read 调用，相对解码开销可以忽略。 */
#define RADIO_HLS_READ_CHUNK_MAX 2048U
/* 分片和播放列表用不同的超时。
 *
 * 分片是几百 KB 的大对象，弱网下建连慢是正常的，超时给宽；播放列表只有 200 字节，
 * 一次拿不到就是真拿不到，没有“再等等就好”的余地。共用一个超时，会让一次列表重拉
 * 白白多等一整个分片的时间。
 *
 * 而超时本身就是静音时长：日志里电台从 49990 哑到 61710、从 61707 哑到 74487，
 * 两次都是十秒级——因为那时只剩个位数帧在队列里垫着（61707 那笔 queued=1）。
 * 对一个 10 s 一片的直播流来说，超时该按“一片的时间”来定：等满一个超时，节目
 * 也就丢掉一片，多等没有任何收益。 */
#define RADIO_HLS_SEGMENT_TIMEOUT_MS 6000
#define RADIO_HLS_PLAYLIST_TIMEOUT_MS 2500
/* 建连和读响应体分开计时。
 *
 * open() 里那一次要拿来做 TCP 握手，弱网下慢是常态，沿用分片的 6 秒；而响应体是
 * 2 KB 一块分次读的，正常一块几十毫秒就到。合并成一个超时的代价是：一次读卡壳就
 * 是整整 6 秒没声音，而队列在追平直播时本来就不厚。拿 2 秒去赌，输了也只是把这一块
 * 交给下一次重试（见 RADIO_HLS_READ_STALL_LIMIT）。 */
#define RADIO_HLS_READ_TIMEOUT_MS 2000
/* 当前分片读超时后原地重试的上限。esp_http_client_read 返回 0 有两种含义，以前被
 * 合并成了同一个分支：
 *   - 响应体读完了——正常收流，该换下一片；
 *   - 读超时——这一片还没下完，跳片等于把没下完的数据扔掉，还会把一次抖动放大成
 *     一次静音。
 * 用 is_complete_data_received() 分开之后，超时改成重试同一片，重试上限就是这里。 */
#define RADIO_HLS_READ_STALL_LIMIT 1U
/* 追平直播后的等待参数。分片 10 秒一个，设备把播放列表里的分片读完就无片可拉，
 * 只能等下一片生成；期间每隔 RELIST_ROUNDS × WAIT_MS（≈1 秒）重拉一次播放列表看
 * 新片出来没有（列表只有 200 字节，代价是一次建连）。 */
#define RADIO_HLS_EDGE_WAIT_MS 200
#define RADIO_HLS_EDGE_RELIST_ROUNDS 5U
/* 边缘等待的总上限：一直等不到新分片就说明源本身出了问题，交给重连去查，
   不能让电台无声无息地挂在这里。 */
#define RADIO_HLS_EDGE_MAX_WAIT_US (30000LL * 1000LL)

/* 断流重连的次数上限。它只在“这次收听一次都没出过声”时才算数——出过声之后
   stream_retry_after_stall() 不再看它（见 s_played_audio 的说明）。 */
#define RADIO_MAX_RECONNECTS 3U
/* 建连阶段（DNS/TCP 握手、拉播放列表、开解码器、抢扬声器）各自的重试上限与间隔。
 *
 * 这些失败以前是一次判死的。实测日志里 getaddrinfo() 返回 202（EAI_AGAIN，DNS 临时
 * 故障）就让电台直接回 IDLE，而四秒前同一份日志里刚发生过三次 select() timeout——
 * 网络正差，但差是临时的。用户看到的结果是“电台自己关了、屏幕退回主页面”。
 *
 * 建连比断流更值得多给几次机会：断流时播放队列还能垫一会儿，建连失败则是当场没
 * 声音。上限给得比 RADIO_MAX_RECONNECTS 宽。
 *
 * 间隔走 retry_backoff_ms()：前几次仍是 500 ms（DNS 抖动通常秒级自愈，等太久反而会
 * 把“重连中”拖成看起来的卡死），越过上限之后翻倍、8 秒封顶——到那一步说明收音机已经
 * 出过声了，它就是无限重连，固定间隔会退化成每几百毫秒一轮的 DNS+TCP 风暴。 */
#define RADIO_MAX_CONNECT_RETRIES 6U
#define RADIO_CONNECT_RETRY_DELAY_MS 500U
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
/* 本次收听（从按下播放到停止之间）有没有真的出过声。它决定断流之后是“无限重连”还是
 * “用完预算就报失败”。
 *
 * 出过声说明用户正在听：网络抖动是他的常态，重连回来就是了。实测日志里电台连着放了
 * 三个多分钟，每次都是“分片读超时 → 追平直播边缘 → 重拉列表失败”，三次重连预算用完
 * 就 record_failure + 清 s_play_requested——用户看到的是“电台自己关了、屏幕退回主页
 * 面”。这不是他要的“稳定”，所以出过声的流不再设上限。
 *
 * 一次都没响过（刚说“打开电台”就接不上）才受预算约束：那种要尽快把失败交给语音侧，
 * 让用户听到一句“连不上”，而不是对着一个永远连不上的电台干等。 */
static bool s_played_audio;
/* 当前频道在 k_stations 里的下标。切台只改它、再让电台重连：重新解析播放列表是必须
   的（新台的分片目录和序号都不同），但回放通道不关，所以用户听到的是无缝切换。 */
static uint32_t s_station_index;

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

/* 电台流的采样率由源端决定（CNR 实测 44.1/48 kHz 立体声），而本地 I2S 固定
   LUMIDOCK_AUDIO_SAMPLE_RATE_HZ(16 kHz) 单声道。不转换就等于把 44.1 kHz 的数据
   按 16 kHz 发出去，声音会变成慢速低音。广播音质用线性插值已经够。

   调用方传的是同一个缓冲（dst == src，见 radio_task 里的调用点），这是安全的：
   输出永远比输入小——立体声→单声道除以 2，44.1/48 kHz→16 kHz 再除以约 3，
   合起来约 1/6——写成 int16 的索引就是“每写 1 个，读指针已前进 6 个”，
   覆写永远追不上还没读的源数据。因此不需要再单独开一块输出缓冲。 */
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
        /* 原地时数据本来就在位置上，不用搬（memcpy 的源目标重叠是未定义行为）。 */
        if (dst != src) memcpy(dst, src, copy);
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
    esp_http_client_handle_t http; /* 整段会话复用的 handle */
    const char *playlist_url;      /* 本会话的播放列表地址（由切台时选定） */
    char base[RADIO_HLS_URL_MAX];  /* 分片目录，含结尾的 '/' */
    uint64_t next_index;           /* 下一个要拉的分片序号 */
    uint64_t last_index;           /* 播放列表里最后一片的序号；0 表示还没拉到列表 */
    bool segment_open;             /* 当前分片的响应还没读完 */
    bool at_live_edge;             /* 已越过最后一片，在等下一片生成 */
    int64_t throttle_window_us;    /* 限速窗口起点；0 表示窗口还没开 */
    size_t throttle_bytes;         /* 本窗口已读到的字节数 */
    size_t throttle_budget;        /* 本窗口的预算，开窗时按播放队列水位算好 */
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
        /* 列表重拉是“重新对齐”的补救动作，卡在它上面既救不回当前分片，又把静音
           时间翻倍，所以走短超时。读完立刻把超时还给分片。 */
        (void)esp_http_client_set_timeout_ms(hls->http, RADIO_HLS_PLAYLIST_TIMEOUT_MS);
        esp_http_client_set_url(hls->http, hls->playlist_url);
        esp_http_client_close(hls->http);
        const bool opened = esp_http_client_open(hls->http, 0) == ESP_OK;
        if (opened) (void)esp_http_client_fetch_headers(hls->http);
        (void)esp_http_client_set_timeout_ms(hls->http, RADIO_HLS_READ_TIMEOUT_MS);
        if (!opened) return false;
    }
    char body[RADIO_HLS_PLAYLIST_MAX];
    size_t used = 0U;
    while (used < sizeof(body) - 1U) {
        const int n = esp_http_client_read(hls->http, body + used,
                                           (int)(sizeof(body) - 1U - used));
        if (n <= 0) break;
        used += (size_t)n;
        /* 播放列表只有几 KB，但同属 HLS 拉取，一并记账，免得流量口径留缺口。 */
        device_data_traffic_add_rx((size_t)n);
    }
    body[used] = '\0';

    /* 分片目录 = 播放列表 URL 去掉最后一个路径段。 */
    strncpy(hls->base, hls->playlist_url, sizeof(hls->base) - 1U);
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
    hls->last_index = last_index;
    /* 只在必须的时候才重新定位起点：
       - 还没有起点（首次拉列表）；
       - 当前进度已经掉出滑动窗口（比窗口最旧的一片还旧，那些分片早 404 了）。
       追平直播后重拉列表只为了刷新 last_index，这里绝不能把 next_index 一把退回
       “窗口倒数第二片”——那等于把刚听过的 20 秒重播一遍。 */
    if (hls->next_index == 0U || hls->next_index < prev_index) {
        hls->next_index = prev_index != 0U ? prev_index : last_index;
    }
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
    /* 列表重拉会把超时临时调短，拉分片前显式调回来，不依赖调用顺序。 */
    (void)esp_http_client_set_timeout_ms(hls->http, RADIO_HLS_SEGMENT_TIMEOUT_MS);
    esp_http_client_set_url(hls->http, url);
    esp_http_client_close(hls->http);
    if (esp_http_client_open(hls->http, 0) != ESP_OK) return false;
    (void)esp_http_client_fetch_headers(hls->http);
    /* 状态码必须在这里判。CNR 对不存在的分片回的是带 keep-alive 的空体 404，不看
       状态码就直接 read，会把整个 RADIO_HLS_SEGMENT_TIMEOUT_MS 等满才返回 0——
       日志里 51517→64017 那 12.5 秒静音就是两次这样的超时。 */
    if (esp_http_client_get_status_code(hls->http) != 200) {
        hls->segment_open = false;
        return false;
    }
    /* 头都拿到了，剩下的是纯读响应体：换成短超时，卡住也只赔 2 秒。 */
    (void)esp_http_client_set_timeout_ms(hls->http, RADIO_HLS_READ_TIMEOUT_MS);
    hls->segment_open = true;
    return true;
}

/* 本窗口的预算：按播放队列水位把拉流速率在“跟住直播”和“追回缺口”之间插值。
 *
 * armed=false（还没抢回扬声器／用户暂停）时水位没有参考意义，直接用基础速率摊平。 */
static size_t hls_throttle_budget(uint32_t epoch, bool armed)
{
    uint32_t rate = RADIO_HLS_TARGET_BYTES_PER_SEC;
    if (armed) {
        const uint32_t pending = audio_playback_service_pending_frames(
            AUDIO_PLAYOUT_SOURCE_RADIO, epoch);
        if (pending <= RADIO_HLS_QUEUE_LOW_FRAMES) {
            rate = RADIO_HLS_CATCHUP_BYTES_PER_SEC;
        } else if (pending < RADIO_HLS_QUEUE_HIGH_FRAMES) {
            const uint32_t span = RADIO_HLS_QUEUE_HIGH_FRAMES - RADIO_HLS_QUEUE_LOW_FRAMES;
            const uint32_t drop = RADIO_HLS_CATCHUP_BYTES_PER_SEC -
                                  RADIO_HLS_TARGET_BYTES_PER_SEC;
            rate = RADIO_HLS_CATCHUP_BYTES_PER_SEC -
                   (drop * (pending - RADIO_HLS_QUEUE_LOW_FRAMES)) / span;
        }
    }
    return (size_t)((rate * (RADIO_HLS_THROTTLE_WINDOW_US / 1000U)) / 1000U);
}

/* 本窗口读超了就睡到窗口末尾，把拉流的瞬时速率压回内容本身的码率。
 *
 * 睡的时候按 50 ms 分片，不是一口气睡满 200 ms：填满循环里那块让位逻辑
 * （s_yielded / s_epoch）要等到这一轮 hls_read 返回才会被检查，电台不能因为限速
 * 就把 socket 攥住整整一个窗口不放。 */
static void hls_throttle(radio_hls_t *hls, size_t bytes, uint32_t epoch, bool armed)
{
    const int64_t now = esp_timer_get_time();
    if (hls->throttle_window_us == 0 ||
        now - hls->throttle_window_us >= RADIO_HLS_THROTTLE_WINDOW_US) {
        hls->throttle_window_us = now;
        hls->throttle_bytes = bytes;
        /* 预算每开一次窗重算一次：水位随时在变，200 ms 的节奏跟得上。 */
        hls->throttle_budget = hls_throttle_budget(epoch, armed);
        return;
    }
    hls->throttle_bytes += bytes;
    if (hls->throttle_bytes <= hls->throttle_budget) return;
    while (esp_timer_get_time() - hls->throttle_window_us <
           RADIO_HLS_THROTTLE_WINDOW_US) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    /* 下一轮重新开窗，不做跨窗口补偿：补偿会让一次抖动之后接着降速，而队列本来
       就该在抖动后尽快追回来。 */
    hls->throttle_window_us = 0;
}

/* HLS 的字节源：把当前分片的响应体当成一个会结束的流，读完了就换下一片。
   返回 >0 为拿到的字节数；返回 0 表示这一轮彻底没进展，交给外层按网络停顿处理。 */
static int hls_read(radio_hls_t *hls, uint8_t *buf, size_t len)
{
    hls->at_live_edge = false;
    uint8_t advance = 0U;
    uint8_t stalls = 0U;
    while (advance < RADIO_HLS_MAX_ADVANCE_PER_READ) {
        if (!hls->segment_open) {
            /* 越过播放列表最后一片＝已经追平直播，那一片还没生成。不在这里判死的话
               会去 open 一个不存在的分片（见 hls_open_segment 的状态码检查），或者
               把一次 read 白挂到一个不会来的 body 上。交给外层去“等新片”，而不是
               当成网络故障重连。

               注意这里是 >= 而不是 >：列表里最后一片通常刚出现在清单上，内容还在
               服务器端写入。抢先拉它只会拿到半片——实测 16329138 拉了 11.3 秒都没
               读完，重试一次后被当成网络故障跳过，用户少听 10 秒。多落后一片
               （10 秒）换稳定，对直播电台完全划算。 */
            if (hls->last_index != 0U && hls->next_index >= hls->last_index) {
                hls->at_live_edge = true;
                return 0;
            }
            if (!hls_open_segment(hls, hls->next_index)) {
                /* 一次失败的 open 已经吃掉了整个超时。这时最不该做的就是再赌两次
                   同样的超时（重拉列表要 open 一次、重开分片又要 open 一次），那会
                   把一次网络抖动放大成三倍静音。

                   先按最常见的原因处理：这一片本身过期/被清理了。直接跳过它试下一
                   片——直播可以丢音，接上新的一片远比补回旧的一片重要。 */
                ESP_LOGW(TAG, "HLS 分片 %llu 拉不到；跳过它试下一片",
                         (unsigned long long)hls->next_index);
                ++hls->next_index;
                ++advance;
                if (!hls_open_segment(hls, hls->next_index)) {
                    /* 连下一片也拿不到，才说明是整体落后到滑动窗口之外了：
                       这时才值得重拉列表重新对齐。 */
                    ESP_LOGW(TAG, "HLS 分片 %llu 仍拉不到；重拉播放列表重新对齐",
                             (unsigned long long)hls->next_index);
                    if (!hls_read_playlist(hls, true)) return 0;
                    /* 重拉之后还是没片可拉：这就是追平了直播，不是网络故障。
                       （列表里已经多出新片的话 next_index 会落在窗口内，不会走
                       这个分支。）这里同样留最后一片不拉，理由见上面的 >= 判断。 */
                    if (hls->next_index >= hls->last_index) {
                        hls->at_live_edge = true;
                        return 0;
                    }
                    if (!hls_open_segment(hls, hls->next_index)) return 0;
                }
            }
            stalls = 0U;
        }
        const int n = esp_http_client_read(hls->http, (char *)buf, (int)len);
        if (n > 0)
        {
            device_data_traffic_add_rx((size_t)n);
            return n;
        }
        /* 返回 0 的两种含义在这里分开：响应体没读完就是读超时，原地重试同一片；
           读完了才收流换下一片。以前合并成一个分支，一次抖动就被当成“片读完”
           跳过去，剩下的数据永远不再补，用户听到的就是一段静音。 */
        if (stalls < RADIO_HLS_READ_STALL_LIMIT &&
            !esp_http_client_is_complete_data_received(hls->http)) {
            ++stalls;
            ESP_LOGW(TAG, "HLS 分片 %llu 读超时未读完（重试 %u/%u）",
                     (unsigned long long)hls->next_index,
                     (unsigned)stalls, (unsigned)RADIO_HLS_READ_STALL_LIMIT);
            continue;
        }
        /* 这一片读完（或重试耗尽）：收流，下一轮换下一片。 */
        hls->segment_open = false;
        ++hls->next_index;
        ++advance;
        stalls = 0U;
    }
    ESP_LOGW(TAG, "HLS 连续 %u 片都没读出数据；按网络停顿处理",
             (unsigned)RADIO_HLS_MAX_ADVANCE_PER_READ);
    return 0;
}

/* 重试之间等多久：还在预算内（≤ RADIO_MAX_CONNECT_RETRIES）就是原来的定值 500 ms，
 * 越过预算之后 1/2/4/8 秒翻倍封顶。
 *
 * 为什么预算内不退避：这几轮的意义是“DNS 抖动通常秒级自愈”，而且预算一用完就要立刻
 * 把失败报给语音侧——指数退避会把“打开电台但连不上”从 3 秒拖到半分钟，让用户对着
 * 一个没反应的设备干等。
 * 为什么越界要退避：越界只可能是出过声的流（无限重连），源长期不可用时不能每几百
 * 毫秒刷一轮 DNS+TCP——内部 RAM 本来就紧张（实测建连期只剩 5 KB），越刷越连不上。 */
static uint32_t retry_backoff_ms(uint8_t failures)
{
    if (failures <= RADIO_MAX_CONNECT_RETRIES) return RADIO_CONNECT_RETRY_DELAY_MS;
    const uint8_t over = (uint8_t)(failures - RADIO_MAX_CONNECT_RETRIES);
    const uint8_t step = over > 4U ? 4U : over;
    return RADIO_CONNECT_RETRY_DELAY_MS << step;
}

/* 断流之后还能不能再接：能就把重连计数推一格并返回真，不能就返回假（由调用方记失败）。
 *
 * 两个判定点（直播边缘等不到新片、普通网络停顿）共用它，保证两处判据永远一致——它们
 * 描述的是同一件事“这条流断了”，任何一处漏改，电台就会从那个口子退出，而且只在特定
 * 网络故障下复现。边缘重拉列表失败不走这里，因为那不算流断了（见调用点）。 */
static bool stream_retry_after_stall(uint8_t *reconnects)
{
    if (*reconnects < RADIO_MAX_RECONNECTS || s_played_audio) {
        if (*reconnects < 0xFFU) ++(*reconnects);
        return true;
    }
    return false;
}

static void radio_task(void *arg)
{
    (void)arg;
    uint8_t *input = heap_caps_malloc(RADIO_INPUT_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *pcm = heap_caps_malloc(RADIO_PCM_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (input == NULL || pcm == NULL) {
        ESP_LOGE(TAG, "radio buffers allocation failed");
        vTaskDelete(NULL);
        return;
    }
    /* 建连阶段的重试计数。它必须活在会话循环之外：建连失败走的是 continue，每次
       都会回到 for(;;) 顶部把循环内的变量重新声明一遍；放在循环里面就等于永远归
       零，重试上限形同虚设。 */
    uint8_t connect_retries = 0U;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool requested = s_play_requested;
        const bool yield_parked = s_yielded;
        const uint32_t epoch = s_epoch;
        const radio_station_t *station = &k_stations[s_station_index];
        xSemaphoreGive(s_lock);
        /* 让位期间不建立也不保持任何流：这正是要让出去的那部分资源。等 regain()
           清掉 s_yielded 再重新接回直播（直播可以丢音，直接接当前时刻）。 */
        if (!requested || yield_parked) {
            /* 台已经不播了（被关掉或被语音让位）＝这一轮建连结束，预算清零。
               connect_retries 是跨会话活着的（见它的声明），不清就会把上一轮用剩的
               次数带到下一次“打开电台”：最坏的情况是那次只剩一两次机会就报连不上，
               而用户什么也没做错。 */
            connect_retries = 0U;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }

        ESP_LOGI(TAG, "radio state=CONNECTING epoch=%u", (unsigned)epoch);
        radio_hls_t hls = {0};
        esp_http_client_config_t cfg = {
            .url = station->url,
            /* 超时从一个坏 socket 里拔出来要等多久。
             *
             * 原来是 4 s，理由是“实测建连只要 240 ms，十几倍余量”。但那 240 ms 是
             * 网络好的时候测的：日志里网络一差（同时段 PONG 从 10 s 拖到 20 s），建连
             * 要好几秒，4 s 必然超时。
             *
             * 后来改成 10 s，并写下“宁可多等，别把本来能连上的时候判死”。实测证明这条
             * 取舍立在一个不成立的前提上：那句“播放队列那几百帧在垫着”并不总成立——
             * 61707 那次下令牌时队列只剩 1 帧，一次卡满就是 12.8 秒静音（61707 → 74487）。
             * 对一个 10 s 一片的直播流来说，超时该按“一片的时间”来定，而不是按“网络
             * 还能慢到多慢”来定：等满一片，节目也丢掉一片，多等没有收益。
             *
             * 6 s 是两者的边界：比原来的 4 s 宽出 50%（保住弱网下不误判），又不会让
             * 一次超时超过一整个分片。判早了还有跳片兜底（见 hls_read），判晚了一整片
             * 就没了。 */
            .timeout_ms = RADIO_HLS_SEGMENT_TIMEOUT_MS,
            .buffer_size = RADIO_HTTP_BUFFER_BYTES,
        };
        esp_http_client_handle_t http = esp_http_client_init(&cfg);
        esp_err_t open_err = http == NULL ? ESP_ERR_NO_MEM : esp_http_client_open(http, 0);
        if (open_err != ESP_OK) {
            /* 建连失败不能一次判死，而且这里清 s_play_requested 的副作用比想象中大：
               它同时是 UI 判断“还在电台模式”的唯一标志（ui_service.c 里那句
               !radio_stream_service_is_requested() 就立刻翻回主页面），清掉等于
               告诉用户这一轮结束了。
               所以重连期间刻意保留 s_play_requested：电台停在“重连中”，屏幕也停在
               语音页，连上就直接接着播。 */
            const char *stage = http == NULL ? "http_init" : "http_open";
            if (http != NULL) esp_http_client_cleanup(http);
            if (connect_retries < RADIO_MAX_CONNECT_RETRIES || s_played_audio) {
                /* 出过声的流不再设上限（见 s_played_audio 的说明），计数封在 uint8_t 里。
                   所以日志会出现“建连重试 9/6”这种读法：分母是名以上限，分子是真实次数，
                   越过 6 就说明这一轮已经切成无限重连了。 */
                if (connect_retries < 0xFFU) ++connect_retries;
                ESP_LOGW(TAG, "radio %s 失败（err=%d）；建连重试 %u/%u",
                         stage, (int)open_err, (unsigned)connect_retries,
                         (unsigned)RADIO_MAX_CONNECT_RETRIES);
                vTaskDelay(pdMS_TO_TICKS(retry_backoff_ms(connect_retries)));
                continue;
            }
            connect_retries = 0U;
            record_failure(stage, open_err, 0);
            xSemaphoreTake(s_lock, portMAX_DELAY); s_play_requested = false; xSemaphoreGive(s_lock);
            audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_RADIO, epoch);
            continue;
        }
        (void)esp_http_client_fetch_headers(http);
        /* HLS：刚打开的就是播放列表本身，直接在这个响应上解析，不用再连一次。 */
        {
            hls.playlist_url = station->url;
            hls.http = http;
            if (!hls_read_playlist(&hls, false)) {
                /* 列表拉不全多半是这一次响应被截断，重开一条连接通常就好。 */
                esp_http_client_close(http); esp_http_client_cleanup(http);
                if (connect_retries < RADIO_MAX_CONNECT_RETRIES || s_played_audio) {
                    if (connect_retries < 0xFFU) ++connect_retries;
                    ESP_LOGW(TAG, "HLS 列表读取失败；建连重试 %u/%u",
                             (unsigned)connect_retries, (unsigned)RADIO_MAX_CONNECT_RETRIES);
                    vTaskDelay(pdMS_TO_TICKS(retry_backoff_ms(connect_retries)));
                    continue;
                }
                connect_retries = 0U;
                record_failure("hls_playlist", ESP_ERR_INVALID_RESPONSE, 0);
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
            .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_TS,
            .dec_cfg = &ts_cfg,
            .cfg_size = sizeof(ts_cfg),
            .use_frame_dec = false,
        };
        esp_audio_simple_dec_handle_t decoder = NULL;
        const esp_audio_err_t dec_open_err = esp_audio_simple_dec_open(&dec_cfg, &decoder);
        if (dec_open_err != ESP_AUDIO_ERR_OK) {
            /* 这一条最常见的成因是内部 RAM 被语音链路临时占住，等一会儿就能开；
               直接判死等于把一次内存抖动变成“电台关闭”。 */
            esp_http_client_close(http); esp_http_client_cleanup(http);
            if (connect_retries < RADIO_MAX_CONNECT_RETRIES || s_played_audio) {
                if (connect_retries < 0xFFU) ++connect_retries;
                ESP_LOGW(TAG, "解码器打开失败（err=%d）；建连重试 %u/%u",
                         (int)dec_open_err, (unsigned)connect_retries,
                         (unsigned)RADIO_MAX_CONNECT_RETRIES);
                vTaskDelay(pdMS_TO_TICKS(retry_backoff_ms(connect_retries)));
                continue;
            }
            connect_retries = 0U;
            record_failure("decoder_open", dec_open_err, 0);
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
                /* 抢不到扬声器通常是因为语音正占着，等它松手就好；同样不能一次判死。 */
                esp_audio_simple_dec_close(decoder); esp_http_client_close(http); esp_http_client_cleanup(http);
                if (connect_retries < RADIO_MAX_CONNECT_RETRIES || s_played_audio) {
                    if (connect_retries < 0xFFU) ++connect_retries;
                    ESP_LOGW(TAG, "回放通道启动失败；建连重试 %u/%u",
                             (unsigned)connect_retries, (unsigned)RADIO_MAX_CONNECT_RETRIES);
                    vTaskDelay(pdMS_TO_TICKS(retry_backoff_ms(connect_retries)));
                    continue;
                }
                connect_retries = 0U;
                record_failure("playback_begin", ESP_FAIL, 0);
                xSemaphoreTake(s_lock, portMAX_DELAY); s_play_requested = false; xSemaphoreGive(s_lock);
                continue;
            }
            armed = true;
            /* 扬声器已经抢到、预缓冲开始灌：从这一刻起这次收听算“出过声”，断流之后
               无限重连（见 s_played_audio 的说明）。放在这里而不是“第一块 PCM 入队成
               功”，是因为两者之间只隔几十毫秒，而这里能覆盖“刚出声就断”的那一档。
               走锁是因为另一个写者是 stop()：它可能和这一句前后脚发生。 */
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_played_audio = true;
            xSemaphoreGive(s_lock);
        }
        /* 建连彻底成功才把重试计数归零。
           这个位置很关键：如果改成“open 成功就归零”，下面 hls_playlist /
           decoder_open / playback_begin 的失败每次都只能把计数推到 1，重试上限
           永远够不着，这里就变成死循环了。 */
        connect_retries = 0U;
        xSemaphoreTake(s_lock, portMAX_DELAY); s_active = true; xSemaphoreGive(s_lock);
        size_t input_len = 0U;
        /* 上一轮解析器实际消化的字节数，决定这一轮是接着喂还是去读。 */
        size_t last_consumed = 0U;
        uint8_t decode_failures = 0U;
        uint8_t stall_rounds = 0U;
        uint8_t queue_full_rounds = 0U;
        /* 追平直播后的等待状态；edge_deadline_us 非 0 就表示正在边缘等待。
           edge_relist_failures 只为不刷屏：边缘重拉已经不是致命错误，网络真断时它会
           每秒失败一次，等满三十秒就是三十行日志。 */
        int64_t edge_deadline_us = 0;
        uint8_t edge_rounds = 0U;
        uint8_t edge_relist_failures = 0U;
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
            /* input_len < 输入缓冲是必须的：缓冲已经满了就没有位置可读，下面的填满
               循环一次都不会跑，filled 会停在 0 而被当成“网络没进展”误报重连。
               那种情况（缓冲满 + 解析器不吃）是上面 stall_rounds 那条路管的。 */
            if ((input_len == 0U || last_consumed == 0U) &&
                input_len < RADIO_INPUT_BUFFER_BYTES) {
                /* HLS 的分片响应体：读完一片自动换下一片。往下的解码/重采样/入队/
                   背压处理完全共用。 */
                /* 小批量读，但读到把缓冲填满为止。
                 *
                 * 上面那段注释解释了为什么不能“只喂一次就回头再读”：读入速度是消化
                 * 的三十几倍，缓冲会一直满、一直整块丢掉。旧 MP3 长连接没暴露它，
                 * 只因为服务端按实时供流、一次 read 只有一两 KB。
                 *
                 * 但反过来把单次读限成 2 KB 然后就走也不行：缓冲永远只装 2 KB，
                 * 解码完就空，每次续杯都得等一次网络往返，实机就是电台一卡一卡。
                 * 预读量是撑过网络抖动的垫子，不能拿去换粒度。
                 *
                 * 所以这里两样都要：每次 read 还是 2 KB（给 WiFi 栈留出调度点），
                 * 但循环到填满 16 KB 才交回主循环。粒度是 2 KB，预读量还是 16 KB。 */
                size_t filled = 0U;
                int n = 0;
                bool relinquish = false;
                while (input_len + filled < RADIO_INPUT_BUFFER_BYTES) {
                    size_t want = RADIO_INPUT_BUFFER_BYTES - input_len - filled;
                    if (want > RADIO_HLS_READ_CHUNK_MAX)
                        want = RADIO_HLS_READ_CHUNK_MAX;
                    n = hls_read(&hls, input + input_len + filled, want);
                    if (n <= 0) break;
                    filled += (size_t)n;
                    /* 分片是整片拉的，这里把它压回内容本身的码率；见 hls_throttle。
                       预算跟着播放队列水位走，队列见底时会自动让开去追缺口。 */
                    hls_throttle(&hls, (size_t)n, epoch, armed);
                    /* 让位必须立刻生效，不能等这一轮把 16 KB 填满。
                       语音抢走扬声器的那一刻（唤醒）正是全网最紧张的时刻：电台要是
                       还接着读网络，就还占着 socket 和 WiFi 接收缓冲，正撞在 WSS 建连
                       上——实测唤醒瞬间建 seq=3 时 largest 只剩 1984 B，就是撞在这里。
                       填满循环里每读一块就回头看一眼状态，该让位就立刻收手。 */
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    relinquish = s_yielded || !s_play_requested || s_epoch != epoch;
                    xSemaphoreGive(s_lock);
                    if (relinquish) break;
                }
                /* 只拿第一拍的结果判成败：填满过程中遇到的 0 多半是这一片读完了
                   （hls_read 内部已经换到下一片），刚拿到的字节不能丢，下一轮接着拉。 */
                if (filled == 0U) {
                    if (n == 0) {
                        /* 追平直播：播放列表里最后一片之后的分片还没生成。这不是网络
                           故障，重连只是多赔上一轮握手；正确做法是在这里等，并每隔
                           一小段重拉列表看新片出来没有。

                           重拉列表只会刷新 last_index，不会把起点退回窗口倒数第二片
                           （见 hls_read_playlist），所以这里不会把听过的内容重播。 */
                        if (hls.at_live_edge) {
                            if (edge_deadline_us == 0) {
                                edge_deadline_us = esp_timer_get_time() + RADIO_HLS_EDGE_MAX_WAIT_US;
                                edge_relist_failures = 0U;
                                ESP_LOGI(TAG, "HLS 已追平直播（分片 %llu 尚未生成）；等新片",
                                         (unsigned long long)hls.next_index);
                            }
                            if (esp_timer_get_time() >= edge_deadline_us) {
                                ESP_LOGW(TAG, "HLS 直播边缘等了 %lld ms 仍无新分片；按网络停顿处理",
                                         (long long)(RADIO_HLS_EDGE_MAX_WAIT_US / 1000LL));
                                if (stream_retry_after_stall(&s_reconnects)) retry = true;
                                else record_failure("http_read_eof", 0, s_reconnects);
                                break;
                            }
                            if (++edge_rounds >= RADIO_HLS_EDGE_RELIST_ROUNDS) {
                                edge_rounds = 0U;
                                if (!hls_read_playlist(&hls, true)) {
                                    /* 重拉失败≠流断了。这里本来就站在直播边缘，真正的问题
                                       是“新片还没生成”，而重拉走的是一次全新的短超时（2.5 s）
                                       建连——网络正好抖一下就会失败，重开一条下一秒就能成。
                                       一次失败就整条重连的代价却是几十秒静音：实测两次
                                       http_read_eof 都是这么来的，边缘只等了 3.3 s 和 7 s，
                                       离 30 s 的上限差得远，而源十几秒后明明就有新片了。
                                       所以这里不判死，接着等下一轮重拉，兜底交给
                                       edge_deadline_us。 */
                                    if (edge_relist_failures == 0U) {
                                        ESP_LOGW(TAG, "HLS 直播边缘重拉列表失败；继续等新片");
                                    }
                                    ++edge_relist_failures;
                                } else {
                                    edge_relist_failures = 0U;
                                }
                            }
                            /* 让位检查交给循环顶部的 keep/yielded 判定：这里直接回顶部，
                               用户唤醒抢扬声器时能立刻停手，不必等满这一轮。 */
                            vTaskDelay(pdMS_TO_TICKS(RADIO_HLS_EDGE_WAIT_MS));
                            continue;
                        }
                        if (stream_retry_after_stall(&s_reconnects)) {
                            retry = true;
                            ESP_LOGW(TAG, "radio network stall; reconnect=%u/%u",
                                     (unsigned)s_reconnects, (unsigned)RADIO_MAX_RECONNECTS);
                        } else {
                            record_failure("http_read_eof", 0, s_reconnects);
                        }
                        break;
                    }
                    record_failure("http_read", n, s_reconnects);
                    break;
                }
                input_len += filled;
                /* 又拿到数据了，边缘等待的计时从头算。 */
                edge_deadline_us = 0;
                edge_rounds = 0U;
                /* 让位/切台要在这一刻收手，但不能带着旧 epoch 继续往下走：下面是
                   解码 + 入队，而用旧 epoch 入队会拿到 ESP_ERR_INVALID_ARG——那条路
                   把它当成“被语音抢占”而置 s_yielded，于是切个台就会把电台误锁进让位
                   态，等一个没人会发的 regain()。把刚读到的字节留在缓冲里直接回循环
                   顶部，由那里的 keep/yielded 决定是转 parked 还是收场。 */
                if (relinquish) continue;
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
                /* 转换是原地的：输出缓冲直接复用 pcm（理由见 radio_to_16k_mono 的注释）。
                   省掉了原来那块 8192 B 的独立输出缓冲，PSRAM 占用从 32 KB 降到 24 KB。 */
                const size_t pcm_bytes =
                    have_info ? radio_to_16k_mono(pcm, out.decoded_size, info.sample_rate,
                                                  info.channel, info.bits_per_sample, pcm,
                                                  RADIO_PCM_BUFFER_BYTES)
                              : out.decoded_size;
                const uint8_t *payload = pcm;
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
            /* 断流重连之间按失败次数退避（见 retry_backoff_ms）。出过声的流现在是无限
               重连的，固定一个很小的间隔会在源长期不可用时退化成忙等——每一轮都要重新
               DNS、TCP、拉播放列表，而它抢的正是紧张的网络和内部 RAM。 */
            vTaskDelay(pdMS_TO_TICKS(retry_backoff_ms(s_reconnects)));
        } else {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            /* 走到这里说明是真正的失败，或是用户 stop()。让位走的是上面的 parked
               分支，不会清播放请求，所以这里可以无条件清掉：否则流已经断了，外层
               循环还会一轮轮重连下去。
               但要认 epoch：stop() 之后用户马上又说“打开电台”时，s_play_requested
               和 epoch 都已经换成新的一轮，这里照着旧 epoch 清一次会把新请求一起
               清掉——电台刚被叫起来就又被按下去，且不会有任何日志。 */
            if (s_epoch == epoch) {
                s_play_requested = false;
                /* 这一轮收听到此为止：下次“打开电台”重新给满预算，否则一个永远连不
                   上的源会被当成“已经出过声”，失败永远报不到语音侧。 */
                s_played_audio = false;
            }
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

/* 切台：改下标 + 换 epoch，让会话循环按新频道重新接流。
 * 故意不碰回放通道（不 abort、不重新 begin），所以“换台”听上去是直接换内容而不是
 * 先断再连；代价是要等新台的播放列表回来才出声（实测约 240 ms）。 */
static esp_err_t radio_select_station(uint32_t index)
{
    if (index >= RADIO_STATION_COUNT) return ESP_ERR_INVALID_ARG;
    if (radio_stream_service_init() != ESP_OK) return ESP_FAIL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool changed = s_station_index != index;
    s_station_index = index;
    ++s_epoch;
    if (s_epoch == 0U) s_epoch = 1U;
    s_play_requested = true;
    s_user_paused = false;
    s_reconnects = 0U;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    if (changed) ESP_LOGI(TAG, "切台：%s（%u/%u）", k_stations[index].name,
                          (unsigned)(index + 1U), (unsigned)RADIO_STATION_COUNT);
    return ESP_OK;
}

esp_err_t radio_stream_service_play_default(void)
{
    return radio_select_station(RADIO_STATION_DEFAULT);
}

/* 台名匹配这条路已经删掉：云端不再点名，用户说"换一个台"就顺着下标走一格。
   留着的名字只用于日志和设备状态，任何选台都不许再走字符串匹配——ASR 的变体
   （"音乐之声儿"）每次都得靠猜，猜错就是"用户说了没反应"。 */
esp_err_t radio_stream_service_play_next(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint32_t next = (s_station_index + 1U) % RADIO_STATION_COUNT;
    xSemaphoreGive(s_lock);
    return radio_select_station(next);
}

const char *radio_stream_service_station_name(void)
{
    if (s_lock == NULL) return k_stations[RADIO_STATION_DEFAULT].name;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *name = k_stations[s_station_index].name;
    xSemaphoreGive(s_lock);
    return name;
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

esp_err_t radio_stream_service_stop(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_play_requested = false;
    s_yielded = false;
    s_user_paused = false;
    /* 用户关电台＝这一段收听结束，重连预算重新记。不清的话，下一次“打开电台”会被
       当成“已经出过声”，遇到一个永远连不上的源就会一直重连下去，失败永远报不到
       语音侧——用户听到的是一片安静的等待，而不是一句“连不上”。 */
    s_played_audio = false;
    ++s_epoch;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

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

/* "用户按了暂停"，和"被语音让位"分开报。两者都是没在响，但界面要说的话完全
   不同：让位期间是语音回合占着扬声器，本来就该显示聆听/思考；用户暂停才是
   "电台停在这儿等你"。is_requested() 在暂停期间就返回假，问不出这一层。 */
bool radio_stream_service_is_user_paused(void) { return s_user_paused; }

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
