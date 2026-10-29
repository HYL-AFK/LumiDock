#include "hourly_chime_service.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "audio_playback_service.h"
#include "esp_log.h"
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

/* 本地素材，攒够 8 帧（240 ms）就开播，和闹钟提示音一致。 */
#define HOURLY_CHIME_PREBUFFER_FRAMES 8U
/* 播放服务单次入队上限是 2048 字节。 */
#define HOURLY_CHIME_CHUNK_BYTES 2048U

typedef struct {
    const uint8_t *start;
    const uint8_t *end;
} hour_pcm_t;

extern const uint8_t prompt_hour_07_start[]
    asm("_binary_prompt_hour_07_16k_mono_s16le_pcm_start");
extern const uint8_t prompt_hour_07_end[]
    asm("_binary_prompt_hour_07_16k_mono_s16le_pcm_end");
extern const uint8_t prompt_hour_08_start[]
    asm("_binary_prompt_hour_08_16k_mono_s16le_pcm_start");
extern const uint8_t prompt_hour_08_end[]
    asm("_binary_prompt_hour_08_16k_mono_s16le_pcm_end");
extern const uint8_t prompt_hour_09_start[]
    asm("_binary_prompt_hour_09_16k_mono_s16le_pcm_start");
extern const uint8_t prompt_hour_09_end[]
    asm("_binary_prompt_hour_09_16k_mono_s16le_pcm_end");
extern const uint8_t prompt_hour_10_start[]
    asm("_binary_prompt_hour_10_16k_mono_s16le_pcm_start");
extern const uint8_t prompt_hour_10_end[]
    asm("_binary_prompt_hour_10_16k_mono_s16le_pcm_end");
extern const uint8_t prompt_hour_11_start[]
    asm("_binary_prompt_hour_11_16k_mono_s16le_pcm_start");
extern const uint8_t prompt_hour_11_end[]
    asm("_binary_prompt_hour_11_16k_mono_s16le_pcm_end");
extern const uint8_t prompt_hour_12_start[]
    asm("_binary_prompt_hour_12_16k_mono_s16le_pcm_start");
extern const uint8_t prompt_hour_12_end[]
    asm("_binary_prompt_hour_12_16k_mono_s16le_pcm_end");
extern const uint8_t prompt_hour_13_start[]
    asm("_binary_prompt_hour_13_16k_mono_s16le_pcm_start");
extern const uint8_t prompt_hour_13_end[]
    asm("_binary_prompt_hour_13_16k_mono_s16le_pcm_end");
extern const uint8_t prompt_hour_14_start[]
    asm("_binary_prompt_hour_14_16k_mono_s16le_pcm_start");
extern const uint8_t prompt_hour_14_end[]
    asm("_binary_prompt_hour_14_16k_mono_s16le_pcm_end");
extern const uint8_t prompt_hour_15_start[]
    asm("_binary_prompt_hour_15_16k_mono_s16le_pcm_start");
extern const uint8_t prompt_hour_15_end[]
    asm("_binary_prompt_hour_15_16k_mono_s16le_pcm_end");
extern const uint8_t prompt_hour_16_start[]
    asm("_binary_prompt_hour_16_16k_mono_s16le_pcm_start");
extern const uint8_t prompt_hour_16_end[]
    asm("_binary_prompt_hour_16_16k_mono_s16le_pcm_end");
extern const uint8_t prompt_hour_17_start[]
    asm("_binary_prompt_hour_17_16k_mono_s16le_pcm_start");
extern const uint8_t prompt_hour_17_end[]
    asm("_binary_prompt_hour_17_16k_mono_s16le_pcm_end");
extern const uint8_t prompt_hour_18_start[]
    asm("_binary_prompt_hour_18_16k_mono_s16le_pcm_start");
extern const uint8_t prompt_hour_18_end[]
    asm("_binary_prompt_hour_18_16k_mono_s16le_pcm_end");
extern const uint8_t prompt_hour_19_start[]
    asm("_binary_prompt_hour_19_16k_mono_s16le_pcm_start");
extern const uint8_t prompt_hour_19_end[]
    asm("_binary_prompt_hour_19_16k_mono_s16le_pcm_end");
extern const uint8_t prompt_hour_20_start[]
    asm("_binary_prompt_hour_20_16k_mono_s16le_pcm_start");
extern const uint8_t prompt_hour_20_end[]
    asm("_binary_prompt_hour_20_16k_mono_s16le_pcm_end");
extern const uint8_t prompt_hour_21_start[]
    asm("_binary_prompt_hour_21_16k_mono_s16le_pcm_start");
extern const uint8_t prompt_hour_21_end[]
    asm("_binary_prompt_hour_21_16k_mono_s16le_pcm_end");

/* 按小时直接索引，不在报时范围的小时留空。用 24 个槽而不是 15 个再算偏移，
 * 是为了让"几点的素材"和"数组第几个"永远对得上，改边界时不用重算下标。 */
static const hour_pcm_t s_hour_pcm[24] = {
    [HOURLY_CHIME_FIRST_HOUR + 0] = {prompt_hour_07_start, prompt_hour_07_end},
    [HOURLY_CHIME_FIRST_HOUR + 1] = {prompt_hour_08_start, prompt_hour_08_end},
    [HOURLY_CHIME_FIRST_HOUR + 2] = {prompt_hour_09_start, prompt_hour_09_end},
    [HOURLY_CHIME_FIRST_HOUR + 3] = {prompt_hour_10_start, prompt_hour_10_end},
    [HOURLY_CHIME_FIRST_HOUR + 4] = {prompt_hour_11_start, prompt_hour_11_end},
    [HOURLY_CHIME_FIRST_HOUR + 5] = {prompt_hour_12_start, prompt_hour_12_end},
    [HOURLY_CHIME_FIRST_HOUR + 6] = {prompt_hour_13_start, prompt_hour_13_end},
    [HOURLY_CHIME_FIRST_HOUR + 7] = {prompt_hour_14_start, prompt_hour_14_end},
    [HOURLY_CHIME_FIRST_HOUR + 8] = {prompt_hour_15_start, prompt_hour_15_end},
    [HOURLY_CHIME_FIRST_HOUR + 9] = {prompt_hour_16_start, prompt_hour_16_end},
    [HOURLY_CHIME_FIRST_HOUR + 10] = {prompt_hour_17_start, prompt_hour_17_end},
    [HOURLY_CHIME_FIRST_HOUR + 11] = {prompt_hour_18_start, prompt_hour_18_end},
    [HOURLY_CHIME_FIRST_HOUR + 12] = {prompt_hour_19_start, prompt_hour_19_end},
    [HOURLY_CHIME_FIRST_HOUR + 13] = {prompt_hour_20_start, prompt_hour_20_end},
    [HOURLY_CHIME_FIRST_HOUR + 14] = {prompt_hour_21_start, prompt_hour_21_end},
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

/* 年-年内日-小时唯一确定一个本地整点。用 tm_yday 而不是 tm_mday，跨月也不用
 * 额外判月；tm_year 从 1900 起算，配合上面的对时门槛恒为正。 */
static long hour_key_from_tm(const struct tm *local)
{
    return ((long)local->tm_year * 1000L + (long)local->tm_yday) * 24L +
           (long)local->tm_hour;
}

static void play_hour(int hour)
{
    const hour_pcm_t *asset = &s_hour_pcm[hour];
    /* 先判空再相减：不在报时范围的槽位是 {NULL, NULL}，对它们做指针差是未定义
     * 行为，虽然实际多半算成 0，但没理由靠这个。 */
    if (asset->start == NULL || asset->end == NULL) {
        ESP_LOGW(TAG, "hour %d has no embedded PCM", hour);
        return;
    }
    if (asset->end <= asset->start) {
        ESP_LOGW(TAG, "hour %d PCM bounds are inverted", hour);
        return;
    }
    const size_t pcm_bytes = (size_t)(asset->end - asset->start);
    if ((pcm_bytes % sizeof(int16_t)) != 0U) {
        ESP_LOGW(TAG, "hour %d PCM is not whole s16le samples (%u bytes)", hour,
                 (unsigned)pcm_bytes);
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

    size_t offset = 0U;
    while (offset < pcm_bytes) {
        const size_t remain = pcm_bytes - offset;
        const size_t chunk = remain > HOURLY_CHIME_CHUNK_BYTES ? HOURLY_CHIME_CHUNK_BYTES : remain;
        if (audio_playback_service_enqueue_pcm(AUDIO_PLAYOUT_SOURCE_HOURLY_CHIME, epoch,
                                               asset->start + offset, chunk) != ESP_OK) {
            ESP_LOGW(TAG, "hour %d enqueue failed at %u/%u bytes", hour,
                     (unsigned)offset, (unsigned)pcm_bytes);
            audio_playback_service_abort(AUDIO_PLAYOUT_SOURCE_HOURLY_CHIME, epoch);
            return;
        }
        offset += chunk;
    }
    (void)audio_playback_service_finish(AUDIO_PLAYOUT_SOURCE_HOURLY_CHIME, epoch);

    ESP_LOGI(TAG, "hour %02d chime queued: %u bytes, epoch=%u", hour,
             (unsigned)pcm_bytes, (unsigned)epoch);
}

esp_err_t hourly_chime_service_init(void)
{
    if (s_ready) return ESP_OK;

    s_epoch = 0U;
    s_last_hour = -1;
    s_settled_key = -1;
    s_pending_hour = -1;
    s_pending_key = 0;
    s_pending_deadline_us = 0;
    s_ready = true;

    ESP_LOGI(TAG, "hourly chime armed: %02d:00 - %02d:00 local, %u assets",
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
        if (local.tm_hour < HOURLY_CHIME_FIRST_HOUR || local.tm_hour > HOURLY_CHIME_LAST_HOUR) {
            /* 报时时段之外：把这一小时标掉，省得这一分钟内每秒都走一遍判断。 */
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
