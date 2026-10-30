#include "hourly_chime_service.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "audio_playback_service.h"
#include "esp_audio_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_opus_dec.h"
#include "esp_timer.h"

static const char *TAG = "hourly_chime";

/* 用户要的是早上 7 点到晚上 9 点，两端都算，所以是 7..21 共 15 个整点。 */
#define HOURLY_CHIME_FIRST_HOUR 7
#define HOURLY_CHIME_LAST_HOUR 21

/* 和 timer_service 用同一个门槛：2020-09-13 之后的时钟才认为是 SNTP 对过时的。
 * 没对时的时候 localtime_r 出来的是 1970 年，凭空报一次时比不报更糟。 */
#define HOURLY_CHIME_SYNCED_EPOCH 1600000000LL

/* 整点被扬声器占着时最多等这么久。超过就放弃这一次：报时是背景信息，
 * 十几分钟后才听见"现在八点"没有意义。 */
#define HOURLY_CHIME_MAX_DEFER_MS 60000

/* 临时：午休时段不报时，免得吵到休息的人。[12, 14) 刚好盖住 12:00 和
 * 13:00 两个整点，14:00 自然恢复，不用再烧一次。
 *
 * 这是今天调试用的时段开关，测完应当删掉；如果要长期保留，应该做成可
 * 配置项而不是写死在这里。 */
#define HOURLY_CHIME_QUIET_FROM_HOUR 12
#define HOURLY_CHIME_QUIET_TO_HOUR 14

/* 本地素材，攒够 8 帧（240 ms）就开播，和闹钟提示音一致。 */
#define HOURLY_CHIME_PREBUFFER_FRAMES 8U
/* 播放服务单次入队上限是 2048 字节；60 ms 的 Opus 包解出来正好 1920 字节，
 * 天然落在上限以内，所以一包解完直接入队，不用再切。 */
#define HOURLY_CHIME_CHUNK_BYTES 2048U

typedef struct {
    const uint8_t *start;
    const uint8_t *end;
} hour_asset_t;

/* 15 个报时素材是 Opus 不是 PCM：语速放慢到 -33% 之后，同一句话裸 PCM 要
 * 78 KB，15 条就是 1.12 MB，而 Opus 只要 5.5 KB 一条。
 *
 * 宏里 ## 拼出符号名，而 asm 标签要先经过 # 变字符串再拼接，所以两层分开写。 */
#define DECLARE_CHIME_ASSET(hour)                                            \
    extern const uint8_t prompt_chime_hour_##hour##_start[]                  \
        asm("_binary_prompt_chime_hour_" #hour "_16k_mono_opus_bin_start"); \
    extern const uint8_t prompt_chime_hour_##hour##_end[]                    \
        asm("_binary_prompt_chime_hour_" #hour "_16k_mono_opus_bin_end")

DECLARE_CHIME_ASSET(07);
DECLARE_CHIME_ASSET(08);
DECLARE_CHIME_ASSET(09);
DECLARE_CHIME_ASSET(10);
DECLARE_CHIME_ASSET(11);
DECLARE_CHIME_ASSET(12);
DECLARE_CHIME_ASSET(13);
DECLARE_CHIME_ASSET(14);
DECLARE_CHIME_ASSET(15);
DECLARE_CHIME_ASSET(16);
DECLARE_CHIME_ASSET(17);
DECLARE_CHIME_ASSET(18);
DECLARE_CHIME_ASSET(19);
DECLARE_CHIME_ASSET(20);
DECLARE_CHIME_ASSET(21);

/* 按小时直接索引，不在报时范围的小时留空。用 24 个槽而不是 15 个再算偏移，
 * 是为了让"几点的素材"和"数组第几个"永远对得上，改边界时不用重算下标。 */
static const hour_asset_t s_hour_asset[24] = {
    [HOURLY_CHIME_FIRST_HOUR + 0] = {prompt_chime_hour_07_start, prompt_chime_hour_07_end},
    [HOURLY_CHIME_FIRST_HOUR + 1] = {prompt_chime_hour_08_start, prompt_chime_hour_08_end},
    [HOURLY_CHIME_FIRST_HOUR + 2] = {prompt_chime_hour_09_start, prompt_chime_hour_09_end},
    [HOURLY_CHIME_FIRST_HOUR + 3] = {prompt_chime_hour_10_start, prompt_chime_hour_10_end},
    [HOURLY_CHIME_FIRST_HOUR + 4] = {prompt_chime_hour_11_start, prompt_chime_hour_11_end},
    [HOURLY_CHIME_FIRST_HOUR + 5] = {prompt_chime_hour_12_start, prompt_chime_hour_12_end},
    [HOURLY_CHIME_FIRST_HOUR + 6] = {prompt_chime_hour_13_start, prompt_chime_hour_13_end},
    [HOURLY_CHIME_FIRST_HOUR + 7] = {prompt_chime_hour_14_start, prompt_chime_hour_14_end},
    [HOURLY_CHIME_FIRST_HOUR + 8] = {prompt_chime_hour_15_start, prompt_chime_hour_15_end},
    [HOURLY_CHIME_FIRST_HOUR + 9] = {prompt_chime_hour_16_start, prompt_chime_hour_16_end},
    [HOURLY_CHIME_FIRST_HOUR + 10] = {prompt_chime_hour_17_start, prompt_chime_hour_17_end},
    [HOURLY_CHIME_FIRST_HOUR + 11] = {prompt_chime_hour_18_start, prompt_chime_hour_18_end},
    [HOURLY_CHIME_FIRST_HOUR + 12] = {prompt_chime_hour_19_start, prompt_chime_hour_19_end},
    [HOURLY_CHIME_FIRST_HOUR + 13] = {prompt_chime_hour_20_start, prompt_chime_hour_20_end},
    [HOURLY_CHIME_FIRST_HOUR + 14] = {prompt_chime_hour_21_start, prompt_chime_hour_21_end},
};

static bool s_ready;
static uint32_t s_epoch;
static int s_last_hour = -1;
/* 已经处理过（报过或跳过）的整点，用"年-年内日-小时"编码。 */
static long s_settled_key = -1;
/* 已经认领、正在等扬声器空出来的整点；-1 表示没有待办。 */
static int s_pending_hour = -1;
static long s_pending_key;
static int64_t s_pending_deadline_us;
/* Opus 解码器和它的输出缓冲。init 时开好，报时那一刻只解码，不碰堆分配。 */
static void *s_decoder;
static uint8_t *s_decode_pcm;

/* 年-年内日-小时唯一确定一个本地整点。用 tm_yday 而不是 tm_mday，跨月也不用
 * 额外判月；tm_year 从 1900 起算，配合上面的对时门槛恒为正。 */
static long hour_key_from_tm(const struct tm *local)
{
    return ((long)local->tm_year * 1000L + (long)local->tm_yday) * 24L +
           (long)local->tm_hour;
}

/* 逐包解码后入队。素材格式是 [u16 包数][u16 x N 包长][包数据连续存放]，
 * 包长是变长的（静音段能被压到 3 字节），所以长度表必须读。 */
static void play_hour(int hour)
{
    const hour_asset_t *asset = &s_hour_asset[hour];
    /* 先判空再相减：不在报时范围的槽位是 {NULL, NULL}，对它们做指针差是未定义
     * 行为，虽然实际多半算成 0，但没理由靠这个。 */
    if (asset->start == NULL || asset->end == NULL) {
        ESP_LOGW(TAG, "hour %d has no embedded Opus", hour);
        return;
    }
    if (asset->end <= asset->start) {
        ESP_LOGW(TAG, "hour %d asset bounds are inverted", hour);
        return;
    }
    const size_t total_bytes = (size_t)(asset->end - asset->start);
    if (total_bytes < 2U) {
        ESP_LOGW(TAG, "hour %d asset is truncated (%u bytes)", hour, (unsigned)total_bytes);
        return;
    }

    const uint16_t packet_count =
        (uint16_t)(asset->start[0] | ((uint16_t)asset->start[1] << 8));
    const size_t header_bytes = 2U + (size_t)packet_count * 2U;
    if (packet_count == 0U || total_bytes < header_bytes) {
        ESP_LOGW(TAG, "hour %d header is inconsistent (%u packets, %u bytes)", hour,
                 (unsigned)packet_count, (unsigned)total_bytes);
        return;
    }
    if (s_decoder == NULL || s_decode_pcm == NULL) {
        ESP_LOGW(TAG, "hour %d skipped: Opus decoder unavailable", hour);
        return;
    }

    /* 播放服务是幂等的，但正常路径上 startup_audio 已经初始化过，这里只是兜底。 */
    if (audio_playback_service_init() != ESP_OK) {
        ESP_LOGW(TAG, "hour %d skipped: playout unavailable", hour);
        return;
    }

    ++s_epoch;
    if (s_epoch == 0U) ++s_epoch;
    const uint32_t epoch = s_epoch;

    if (audio_playback_service_begin(AUDIO_PLAYOUT_SOURCE_HOURLY_CHIME, epoch,
                                     HOURLY_CHIME_PREBUFFER_FRAMES) != ESP_OK) {
        ESP_LOGW(TAG, "hour %d skipped: playout busy", hour);
        return;
    }

    const uint8_t *packet = asset->start + header_bytes;
    size_t decoded_total = 0U;
    for (uint16_t index = 0U; index < packet_count; ++index) {
        const size_t length = (size_t)(asset->start[2U + index * 2U] |
                                       ((uint16_t)asset->start[3U + index * 2U] << 8));
        if (length == 0U || (size_t)(asset->end - packet) < length) {
            ESP_LOGW(TAG, "hour %d packet %u/%u is out of bounds", hour,
                     (unsigned)(index + 1U), (unsigned)packet_count);
            audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_HOURLY_CHIME, epoch);
            return;
        }

        esp_audio_dec_in_raw_t input = {
            .buffer = (uint8_t *)(uintptr_t)packet,
            .len = (uint32_t)length,
        };
        esp_audio_dec_out_frame_t output = {
            .buffer = s_decode_pcm,
            .len = HOURLY_CHIME_CHUNK_BYTES,
            .decoded_size = 0U,
        };
        esp_audio_dec_info_t info = {0};
        const esp_audio_err_t result = esp_opus_dec_decode(s_decoder, &input, &output, &info);
        if (result != ESP_AUDIO_ERR_OK || output.decoded_size == 0U) {
            ESP_LOGW(TAG, "hour %d packet %u/%u decode failed: %d", hour,
                     (unsigned)(index + 1U), (unsigned)packet_count, (int)result);
            audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_HOURLY_CHIME, epoch);
            return;
        }
        if (audio_playback_service_enqueue_pcm(AUDIO_PLAYOUT_SOURCE_HOURLY_CHIME, epoch,
                                               s_decode_pcm, output.decoded_size) != ESP_OK) {
            ESP_LOGW(TAG, "hour %d enqueue failed at packet %u/%u", hour,
                     (unsigned)(index + 1U), (unsigned)packet_count);
            audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_HOURLY_CHIME, epoch);
            return;
        }
        decoded_total += output.decoded_size;
        packet += length;
    }
    (void)audio_playback_service_finish(AUDIO_PLAYOUT_SOURCE_HOURLY_CHIME, epoch);

    ESP_LOGI(TAG, "hour %02d chime queued: %u packets, %u bytes PCM, epoch=%u", hour,
             (unsigned)packet_count, (unsigned)decoded_total, (unsigned)epoch);
}

esp_err_t hourly_chime_service_init(void)
{
    if (s_ready) return ESP_OK;

    /* 解码器跟着 init 一起开，报时那一刻只做解码，不碰堆分配。解码输出固定
     * 1920 字节，放 PSRAM 就够，不必挤占内部 RAM。
     *
     * 开不出来只禁用报时，不返回错误：报时是锦上添花的功能，不能让一个
     * 解码器打不开就把整条启动流程拖停。 */
    if (s_decoder == NULL) {
        if (esp_opus_dec_open(&(esp_opus_dec_cfg_t){
                .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
                .channel = ESP_AUDIO_MONO,
                .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS,
                .self_delimited = false,
            }, sizeof(esp_opus_dec_cfg_t), &s_decoder) != ESP_AUDIO_ERR_OK) {
            s_decoder = NULL;
            ESP_LOGW(TAG, "Opus decoder unavailable; hourly chime disabled");
            return ESP_OK;
        }
    }
    if (s_decode_pcm == NULL) {
        s_decode_pcm = heap_caps_malloc(HOURLY_CHIME_CHUNK_BYTES,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_decode_pcm == NULL) {
            ESP_LOGW(TAG, "no PSRAM for the decode buffer; hourly chime disabled");
            return ESP_OK;
        }
    }

    s_epoch = 0U;
    s_last_hour = -1;
    s_settled_key = -1;
    s_pending_hour = -1;
    s_pending_key = 0;
    s_pending_deadline_us = 0;
    s_ready = true;

    ESP_LOGI(TAG, "hourly chime armed: %02d:00 - %02d:00 local, %u Opus assets",
             HOURLY_CHIME_FIRST_HOUR, HOURLY_CHIME_LAST_HOUR,
             (unsigned)(HOURLY_CHIME_LAST_HOUR - HOURLY_CHIME_FIRST_HOUR + 1));
    return ESP_OK;
}

void hourly_chime_service_poll(void)
{
    if (!s_ready) return;

    const time_t now = time(NULL);
    if ((long long)now <= HOURLY_CHIME_SYNCED_EPOCH) return;

    struct tm local;
    if (localtime_r(&now, &local) == NULL) return;

    const long key = hour_key_from_tm(&local);

    /* 只在整点那一分钟内认领。晚开机正好是 08:37 的话不补报，否则每次上电都会
     * 蹦出一句"现在八点"，听起来像 bug。 */
    if (s_pending_hour < 0 && local.tm_min == 0 && key != s_settled_key) {
        if (local.tm_hour < HOURLY_CHIME_FIRST_HOUR || local.tm_hour > HOURLY_CHIME_LAST_HOUR ||
            (local.tm_hour >= HOURLY_CHIME_QUIET_FROM_HOUR &&
             local.tm_hour < HOURLY_CHIME_QUIET_TO_HOUR)) {
            /* 报时时段之外，或者正处在午休静音窗：把这一小时标掉，
             * 省得这一分钟内每秒都走一遍判断。 */
            s_settled_key = key;
            return;
        }
        s_pending_hour = local.tm_hour;
        s_pending_key = key;
        s_pending_deadline_us = esp_timer_get_time() + (int64_t)HOURLY_CHIME_MAX_DEFER_MS * 1000LL;
        ESP_LOGI(TAG, "hour %02d:00 reached; waiting for the speaker", local.tm_hour);
    }

    if (s_pending_hour < 0) return;

    /* 扬声器被别的会话占着（TTS、本地提示音、电台、闹钟）就先等。播放服务只有
     * 一条通道，硬插进去会把对方当场掐掉，比不报时难听得多。 */
    if (audio_playback_service_active_source() != AUDIO_PLAYOUT_SOURCE_NONE) {
        if (esp_timer_get_time() < s_pending_deadline_us) return;
        ESP_LOGW(TAG, "hour %02d dropped: speaker busy for %d s", s_pending_hour,
                 HOURLY_CHIME_MAX_DEFER_MS / 1000);
        s_settled_key = s_pending_key;
        s_pending_hour = -1;
        return;
    }

    const int hour = s_pending_hour;
    s_settled_key = s_pending_key;
    s_pending_hour = -1;
    s_last_hour = hour;
    play_hour(hour);
}

int hourly_chime_service_last_hour(void)
{
    return s_last_hour;
}
