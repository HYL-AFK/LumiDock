#include "voice_iot_command_service.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "device_settings_service.h"
#include "i2s_audio_driver.h"
#include "mcp_service.h"
#include "radio_stream_service.h"
#include "status_led_service.h"
#include "timer_service.h"
#include "ui_service.h"

static bool contains_any(const char *text, const char *const *phrases, size_t phrase_count)
{
    if (text == NULL) return false;
    for (size_t index = 0; index < phrase_count; ++index) {
        if (strstr(text, phrases[index]) != NULL) return true;
    }
    return false;
}

static bool contains_negation(const char *text)
{
    static const char *const negations[] = {"不", "不要", "不用", "别", "不要调"};
    return contains_any(text, negations, sizeof(negations) / sizeof(negations[0]));
}

/* UTF-8 punctuation length in bytes, 0 when the text does not start with one. */
static size_t utf8_punctuation_len(const char *text)
{
    static const char *const punctuation[] = {
        "。", "，", "！", "？", "、", "；", "：", "“", "”", "‘", "’",
        "（", "）", "《", "》", "【", "】", "…", "—", "～", "·", "．",
    };
    for (size_t index = 0; index < sizeof(punctuation) / sizeof(punctuation[0]); ++index)
    {
        const size_t length = strlen(punctuation[index]);
        if (strncmp(text, punctuation[index], length) == 0) return length;
    }
    return 0U;
}

/* 云端 ASR 只发下来残句时唯一的抓手。现场实测到三种形态：
 *   "电台。"     —— 动词被吞掉
 *   "B电台。"    —— 音乐串进麦克风，切掉的音节变成 ASCII 噪声
 *   "市电台。"   —— 切掉的音节被 ASR 猜成了一个不相干的汉字
 * 短语表全部落空，云端模型只好自己挑动词——它挑“打开”，把用户刚要求停掉的
 * 电台又拉起来。所以这里刻意不用短语表，只看句式。
 *
 * 规则：允许“电台”前面挂少量噪声字（那是被切开头的残留），但“电台”之后
 * 除了重复的“电台”和标点之外不得再有实词。这样：
 *   "市电台。" / "B电台。" / "电台。"  -> 停止
 *   "电台关小点"                        -> 不是（实词在电台之后：调音量）
 *   "把电台关小点"                      -> 不是
 *   "电台有什么节目"                    -> 不是，留给云端
 * 仍然只兜底停止，其余照旧交给云端。 */
#define RADIO_KEYWORD_LEADING_NOISE_MAX 3U
static bool radio_keyword_only(const char *text)
{
    if (text == NULL) return false;
    bool keyword_seen = false;
    size_t leading_noise = 0U;
    for (const char *p = text; *p != '\0';)
    {
        if (strncmp(p, "电台", 6) == 0)
        {
            keyword_seen = true;
            p += 6;
            continue;
        }
        const unsigned char byte = (unsigned char)*p;
        if (byte <= 0x7EU)
        {
            /* 空格、ASCII 标点，以及音乐串进来时 ASR 凭空多写的字母数字：都是噪声。 */
            ++p;
            continue;
        }
        const size_t punctuation = utf8_punctuation_len(p);
        if (punctuation != 0U)
        {
            p += punctuation;
            continue;
        }
        /* 实词。只允许出现在“电台”之前，而且要有预算：一个汉字正好花掉 3 个
           字节，所以预算 3 就是“最多一个被切开头的残留字”。出现在“电台”之后
           说明这句话还有别的意思，交给云端。
           按字节步进而不是 p += 3：遇到 2 字节字符时 +3 会推过结尾的 '\0'。 */
        if (keyword_seen || leading_noise >= RADIO_KEYWORD_LEADING_NOISE_MAX) return false;
        ++leading_noise;
        ++p;
    }
    return keyword_seen;
}

/* ------------------------------------------------------------------ */
/* Number / time parsing for timer commands (Arabic + Chinese digits). */
/* ------------------------------------------------------------------ */

static bool chinese_digit_value(const char *text, uint32_t *value_out)
{
    static const struct {
        const char *utf8;
        uint32_t value;
    } k_digits[] = {
        {"零", 0U}, {"一", 1U}, {"二", 2U}, {"两", 2U}, {"三", 3U}, {"四", 4U},
        {"五", 5U}, {"六", 6U}, {"七", 7U}, {"八", 8U}, {"九", 9U},
    };
    for (size_t i = 0; i < sizeof(k_digits) / sizeof(k_digits[0]); ++i) {
        if (strncmp(text, k_digits[i].utf8, 3) == 0) {
            *value_out = k_digits[i].value;
            return true;
        }
    }
    return false;
}

/* Parses one number (Chinese 0-99 or ASCII digits) at *cursor and advances
 * the cursor past it.  Leaves *cursor untouched on failure. */
static bool parse_number(const char **cursor, uint32_t *value_out)
{
    const char *p = *cursor;
    uint32_t value = 0U;
    size_t used = 0U;
    if (*p >= '0' && *p <= '9') {
        char digits[8];
        size_t len = 0U;
        while (p[len] >= '0' && p[len] <= '9' && len < sizeof(digits) - 1U) {
            digits[len] = p[len];
            ++len;
        }
        digits[len] = '\0';
        value = (uint32_t)strtoul(digits, NULL, 10);
        used = len;
    } else if (strncmp(p, "十", 3) == 0) {
        /* 十 = 10, 十五 = 15 */
        value = 10U;
        used = 3U;
        uint32_t ones = 0U;
        if (chinese_digit_value(p + 3, &ones)) {
            value = 10U + ones;
            used = 6U;
        }
    } else {
        uint32_t tens = 0U;
        if (!chinese_digit_value(p, &tens)) return false;
        value = tens;
        used = 3U;
        if (strncmp(p + 3, "十", 3) == 0) {
            value = tens * 10U;
            used = 6U;
            uint32_t ones = 0U;
            if (chinese_digit_value(p + 6, &ones)) {
                value += ones;
                used = 9U;
            }
        }
    }
    *cursor = p + used;
    *value_out = value;
    return true;
}

/* Hour/minute at the cursor: "7点"/"七点半"/"7:30"/"十九点十五分"... */
static bool parse_clock_hh_mm(const char *text, uint8_t *hour_out,
                              uint8_t *minute_out)
{
    const char *p = text;
    uint32_t hour = 0U;
    if (!parse_number(&p, &hour) || hour > 23U) return false;
    if (strncmp(p, "点", 3) == 0) {
        p += 3;
    } else if (*p == ':') {
        ++p;
    } else if (strncmp(p, "：", 3) == 0) {
        p += 3;
    } else {
        return false;
    }
    uint32_t minute = 0U;
    if (strncmp(p, "半", 3) == 0) {
        minute = 30U;
    } else if (parse_number(&p, &minute) && minute <= 59U) {
        if (strncmp(p, "分", 3) == 0) p += 3;
    }
    *hour_out = (uint8_t)hour;
    *minute_out = (uint8_t)minute;
    return true;
}

/* Scans the whole text for the first clock expression.  "下午/晚上" shifts
 * hours 1-11 to 13-23. */
static bool find_clock_in_text(const char *text, uint8_t *hour_out,
                               uint8_t *minute_out)
{
    const bool pm = strstr(text, "下午") != NULL || strstr(text, "晚上") != NULL;
    for (const char *p = text; *p != '\0';) {
        if (parse_clock_hh_mm(p, hour_out, minute_out)) {
            if (pm && *hour_out >= 1U && *hour_out <= 11U) {
                *hour_out = (uint8_t)(*hour_out + 12U);
            }
            return true;
        }
        if (((unsigned char)*p & 0x80U) != 0U) p += 3; else ++p;
    }
    return false;
}

/* Accumulates every "N小时/N分钟/N秒钟/N秒" fragment into total seconds.
 * Supports 半小时/半分钟/一个半小时 and a 7-day safety cap. */
#define MAX_DURATION_SECONDS (7U * 24U * 3600U)

static bool parse_duration_seconds(const char *text, uint32_t *seconds_out)
{
    uint32_t total = 0U;
    bool found = false;
    for (const char *p = text; *p != '\0';) {
        if (strncmp(p, "半小时", 9) == 0) {
            total += 1800U;
            found = true;
            p += 9;
            continue;
        }
        if (strncmp(p, "半分钟", 9) == 0) {
            total += 30U;
            found = true;
            p += 9;
            continue;
        }
        uint32_t value = 0U;
        const char *next = p;
        if (!parse_number(&next, &value)) {
            if (((unsigned char)*p & 0x80U) != 0U) p += 3; else ++p;
            continue;
        }
        if (value > 10000U) return false;
        p = next;
        if (strncmp(p, "个", 3) == 0) {
            p += 3;
            /* 一个半小时 = 1h + 30m, 一个半分钟 = 1m + 30s */
            if (strncmp(p, "半", 3) == 0) {
                if (strncmp(p + 3, "小时", 6) == 0) {
                    total += value * 3600U + 1800U;
                    found = true;
                    p += 9;
                    continue;
                }
                if (strncmp(p + 3, "分钟", 6) == 0) {
                    total += value * 60U + 30U;
                    found = true;
                    p += 9;
                    continue;
                }
            }
        }
        if (strncmp(p, "小时", 6) == 0) {
            total += value * 3600U; found = true; p += 6; continue;
        }
        if (strncmp(p, "钟头", 6) == 0) {
            total += value * 3600U; found = true; p += 6; continue;
        }
        if (strncmp(p, "分钟", 6) == 0) {
            total += value * 60U; found = true; p += 6; continue;
        }
        if (strncmp(p, "秒钟", 6) == 0) {
            total += value; found = true; p += 6; continue;
        }
        if (strncmp(p, "分半", 6) == 0) {
            total += value * 60U + 30U; found = true; p += 6; continue;
        }
        if (strncmp(p, "分", 3) == 0 && strncmp(p + 3, "钟", 3) != 0) {
            total += value * 60U; found = true; p += 3; continue;
        }
        if (strncmp(p, "秒", 3) == 0) {
            total += value; found = true; p += 3; continue;
        }
        if (total > MAX_DURATION_SECONDS) return false;
    }
    if (!found || total == 0U || total > MAX_DURATION_SECONDS) return false;
    *seconds_out = total;
    return true;
}

/* "音量调到 30%" / "音量设为百分之八十" -> 30 / 80.  Returns false when no
 * phrase is followed by a parseable level. */
static bool parse_volume_level(const char *text, const char *const *phrases,
                               size_t phrase_count, float *percent_out)
{
    for (size_t index = 0; index < phrase_count; ++index) {
        const char *cursor = strstr(text, phrases[index]);
        if (cursor == NULL) continue;
        cursor += strlen(phrases[index]);
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (strncmp(cursor, "百分之", 9) == 0) cursor += 9;
        uint32_t value = 0U;
        if (!parse_number(&cursor, &value)) continue;
        if (value > 100U) value = 100U;
        *percent_out = (float)value;
        return true;
    }
    return false;
}

/* Applies an absolute speaker volume and remembers it across reboots.  The
 * definition sits below the dispatcher on purpose: the contract test asserts
 * that the cloud stand-down (mcp_service_is_ready) is read before the first
 * local driver call in this file. */
static voice_iot_command_result_t apply_local_volume(const char *transcript, float target);

/* Fills the caller's announce buffer (a no-op when announce is NULL). */
static void set_announce(char *announce, size_t announce_size, const char *format, ...)
{
    if (announce == NULL || announce_size == 0U) return;
    va_list args;
    va_start(args, format);
    vsnprintf(announce, announce_size, format, args);
    va_end(args);
    announce[announce_size - 1U] = '\0';
}

voice_iot_command_result_t voice_iot_command_service_execute(
    const char *transcript, char *announce, size_t announce_size)
{
    static const char *const close_led_phrases[] = {"关闭灯带", "关灯带"};
    static const char *const open_led_phrases[] = {"打开灯带", "开灯带"};
    static const char *const exit_standby_phrases[] = {"退出待机", "唤醒屏幕"};
    static const char *const volume_up_phrases[] = {
        "大音量", "声音大一点", "音量大一点", "音量增加", "调高音量", "提高音量",
    };
    static const char *const volume_down_phrases[] = {
        "小音量", "声音小一点", "音量小一点", "音量减少", "调低音量", "降低音量",
    };
    /* "把音量调到 30" 这类绝对音量口令：短语后面直接跟百分比。 */
    static const char *const volume_level_phrases[] = {
        "音量调到", "音量调成", "音量调至", "音量调到", "音量设为", "音量设成",
        "音量设置为", "音量调整到", "音量降到", "音量降为", "声音调到", "声音调成",
        "声音设为", "音量到",
    };
    static const char *const volume_max_phrases[] = {
        "音量调到最大", "音量调到最大声", "音量最大", "声音调到最大",
    };
    static const char *const volume_min_phrases[] = {
        "音量调到最小", "音量调到最小声", "音量最小", "声音调到最小",
    };
    /* 云端 ASR 会把动词首字吃掉（实测"打开电台"→"开电台"、"关闭电台"→"关
       电台"），所以短形式也一并收进来，别指望云端总把整句发下来。 */
    static const char *const radio_play_phrases[] = {
        "放电台", "播放电台", "打开电台", "开电台", "打开一下电台", "开一下电台",
    };
    /* “换一个台”类：四个频道里循环，用户不用记台名。长形式必须单列——
       “换一个台”里并没有连续的“换台”两个字，只收短形式会把它漏掉。 */
    static const char *const radio_next_phrases[] = {
        "换一个台", "换一个电台", "换个台", "换个电台", "换台", "换电台",
        "换个频道", "换频道", "下一个台", "下一个电台", "下一台",
        "切换电台", "切换台", "切台",
    };
    static const char *const radio_pause_phrases[] = {"暂停电台", "暂停播放"};
    /* 只收“截断形”和完整词，不设“电台关”这类宽前缀：它会误伤
       “把电台关小点”这种还没说完的调音量话。 */
    static const char *const radio_stop_phrases[] = {
        "停止电台", "关闭电台", "关电台", "关掉电台", "电台停止", "电台关闭", "电台关掉",
    };
    static const char *const stop_ring_phrases[] = {
        "关掉闹钟", "关闭闹钟", "停止响铃", "停闹钟", "停止闹钟", "别响了",
    };
    static const char *const query_countdown_phrases[] = {
        "还剩多久", "还剩多少", "还有多久", "倒计时还剩", "倒计时还有",
    };
    static const char *const query_stopwatch_phrases[] = {
        "计时多久", "正计时多久", "计了多久", "计时多长时间",
    };
    static const char *const stop_stopwatch_phrases[] = {
        "停止计时", "结束计时", "停止正计时", "结束正计时",
    };
    static const char *const pause_stopwatch_phrases[] = {
        "暂停计时", "暂停正计时",
    };
    static const char *const start_stopwatch_phrases[] = {
        "开始计时", "开始正计时", "正计时开始",
    };
    static const char *const countdown_phrases[] = {"倒计时"};
    static const char *const alarm_phrases[] = {
        "闹钟", "提醒我", "叫我", "喊我", "叫醒我",
    };

    if (contains_negation(transcript) &&
        (strstr(transcript, "音量") != NULL || strstr(transcript, "声音") != NULL)) {
        ESP_LOGW("voice_iot", "local IoT command rejected: text=%s reason=negation", transcript);
        return VOICE_IOT_COMMAND_NOT_HANDLED;
    }

    if (contains_any(transcript, close_led_phrases,
                     sizeof(close_led_phrases) / sizeof(close_led_phrases[0]))) {
        return status_led_service_set_user_enabled(false) == ESP_OK ?
                   VOICE_IOT_COMMAND_EXECUTED : VOICE_IOT_COMMAND_FAILED;
    }
    if (contains_any(transcript, open_led_phrases,
                     sizeof(open_led_phrases) / sizeof(open_led_phrases[0]))) {
        return status_led_service_set_user_enabled(true) == ESP_OK ?
                   VOICE_IOT_COMMAND_EXECUTED : VOICE_IOT_COMMAND_FAILED;
    }
    if (contains_any(transcript, exit_standby_phrases,
                     sizeof(exit_standby_phrases) / sizeof(exit_standby_phrases[0]))) {
        return ui_service_request_exit_standby() == ESP_OK ?
                   VOICE_IOT_COMMAND_EXECUTED : VOICE_IOT_COMMAND_FAILED;
    }

    /* Once the cloud has discovered the MCP tools it resolves a spoken level into
     * its own set_volume call.  Applying the shortcut as well would move the
     * speaker twice and the spoken answer would stop matching what was actually
     * heard.  Every command the cloud tools can serve stands down for the same
     * reason.
     *
     * 但“相对音量”不能算在里面。“调高音量”让位出去会掉进一个两边都不管的缝：
     * 设备让位后不再步进，而 self.audio_speaker.set_volume 的工具描述明写着
     * "instead of stepping the volume up or down"，云端因此也不会调它。实测在
     * 最大音量下说“调高音量”就是这样——本地不动、云端不答，只剩一句退出语。
     *
     * 相对调节归设备还有一条更硬的理由：只有设备知道当前值（get_output_volume），
     * 云端只能先查一次 get_device_status 再自己算，漏查就只能猜——猜低了就表现
     * 为“降低音量”把声音一下拉到静音。
     *
     * 所以让位的只有“绝对音量”：用户报了具体数值（音量调到30）或极值（最大/最小），
     * 云端按契约直接 set_volume，设备不插手。 */
    const bool stop_ring_requested =
        contains_any(transcript, stop_ring_phrases,
                     sizeof(stop_ring_phrases) / sizeof(stop_ring_phrases[0]));
    const bool volume_absolute_requested =
        contains_any(transcript, volume_level_phrases, sizeof(volume_level_phrases) / sizeof(volume_level_phrases[0])) ||
        contains_any(transcript, volume_max_phrases, sizeof(volume_max_phrases) / sizeof(volume_max_phrases[0])) ||
        contains_any(transcript, volume_min_phrases, sizeof(volume_min_phrases) / sizeof(volume_min_phrases[0]));
    const bool timer_requested =
        contains_any(transcript, countdown_phrases, sizeof(countdown_phrases) / sizeof(countdown_phrases[0])) ||
        contains_any(transcript, alarm_phrases, sizeof(alarm_phrases) / sizeof(alarm_phrases[0])) ||
        contains_any(transcript, start_stopwatch_phrases, sizeof(start_stopwatch_phrases) / sizeof(start_stopwatch_phrases[0])) ||
        contains_any(transcript, pause_stopwatch_phrases, sizeof(pause_stopwatch_phrases) / sizeof(pause_stopwatch_phrases[0])) ||
        contains_any(transcript, stop_stopwatch_phrases, sizeof(stop_stopwatch_phrases) / sizeof(stop_stopwatch_phrases[0])) ||
        contains_any(transcript, query_countdown_phrases, sizeof(query_countdown_phrases) / sizeof(query_countdown_phrases[0])) ||
        contains_any(transcript, query_stopwatch_phrases, sizeof(query_stopwatch_phrases) / sizeof(query_stopwatch_phrases[0]));
    const bool radio_requested =
        contains_any(transcript, radio_play_phrases, sizeof(radio_play_phrases) / sizeof(radio_play_phrases[0])) ||
        contains_any(transcript, radio_next_phrases, sizeof(radio_next_phrases) / sizeof(radio_next_phrases[0])) ||
        contains_any(transcript, radio_pause_phrases, sizeof(radio_pause_phrases) / sizeof(radio_pause_phrases[0])) ||
        contains_any(transcript, radio_stop_phrases, sizeof(radio_stop_phrases) / sizeof(radio_stop_phrases[0]));
    /* Silencing a ringing timer stays local whatever the cloud offers: the
     * chime has to stop now, not after a websocket round trip.  The radio is
     * local as well: the cloud model kept answering "打开电台" with a chat
     * about genres instead of calling self.radio.play, so the station has to
     * be started here or it never plays at all.
     * The stand-down itself still reports a command (VOICE_IOT_COMMAND_DEFERRED)
     * so the caller confirms with "正在执行" rather than "正在思考". */
    if (mcp_service_is_ready() && !stop_ring_requested &&
        (volume_absolute_requested || timer_requested)) {
        ESP_LOGI("voice_iot",
                 "command deferred to the cloud MCP tool: text=%s volume=%d timer=%d radio=%d",
                 transcript, volume_absolute_requested ? 1 : 0, timer_requested ? 1 : 0,
                 radio_requested ? 1 : 0);
        return VOICE_IOT_COMMAND_DEFERRED;
    }
    /* "音量调到最大/最小" carry no number and share the "音量调到" prefix with the
     * absolute form below, so they must be matched first. */
    if (contains_any(transcript, volume_max_phrases,
                     sizeof(volume_max_phrases) / sizeof(volume_max_phrases[0]))) {
        return apply_local_volume(transcript, 100.0f);
    }
    if (contains_any(transcript, volume_min_phrases,
                     sizeof(volume_min_phrases) / sizeof(volume_min_phrases[0]))) {
        return apply_local_volume(transcript, 0.0f);
    }
    if (contains_any(transcript, volume_up_phrases, sizeof(volume_up_phrases) / sizeof(volume_up_phrases[0]))) {
        const float before = i2s_audio_driver_get_output_volume();
        /* 已经在顶上：不再写 codec（也不会白白请求一次 NVS 落盘），但必须仍然
           报 EXECUTED，否则 voice service 会退回“正在思考”提示。真正说出口的
           “已经是最大声了”由云端的文字回复负责——工具描述已经要求云端不要为
           相对请求调 set_volume，它只会用话回答。 */
        if (before >= 100.0f) {
            ESP_LOGI("voice_iot", "volume already at maximum: text=%s", transcript);
            return VOICE_IOT_COMMAND_EXECUTED;
        }
        const float after = before + 10.0f > 100.0f ? 100.0f : before + 10.0f;
        return apply_local_volume(transcript, after);
    }
    if (contains_any(transcript, volume_down_phrases, sizeof(volume_down_phrases) / sizeof(volume_down_phrases[0]))) {
        const float before = i2s_audio_driver_get_output_volume();
        if (before <= 0.0f) {
            ESP_LOGI("voice_iot", "volume already at minimum: text=%s", transcript);
            return VOICE_IOT_COMMAND_EXECUTED;
        }
        const float after = before - 10.0f < 0.0f ? 0.0f : before - 10.0f;
        return apply_local_volume(transcript, after);
    }
    /* "把音量调到30" / "音量设为百分之八十": the number follows the phrase. */
    {
        float target = 0.0f;
        if (parse_volume_level(transcript, volume_level_phrases,
                               sizeof(volume_level_phrases) / sizeof(volume_level_phrases[0]),
                               &target)) {
            return apply_local_volume(transcript, target);
        }
    }
    /* 换台判在开台前面：两张短语表没有交集，先后不影响结果，但换台更具体，
       放前面读起来更清楚。 */
    if (contains_any(transcript, radio_next_phrases, sizeof(radio_next_phrases) / sizeof(radio_next_phrases[0]))) {
        /* 和开台共用同一条延迟路径，理由也一样：那段应答 TTS 占着共享播放
         * 通道，当场切过去只会被它静音，用户听到的是"换了但没声"。 */
        set_announce(announce, announce_size, "好的，正在为你换台");
        return VOICE_IOT_COMMAND_RADIO_NEXT;
    }
    if (contains_any(transcript, radio_play_phrases, sizeof(radio_play_phrases) / sizeof(radio_play_phrases[0]))) {
        /* Acknowledge through the cloud reply, then start the stream after it
         * finishes; otherwise begin_tts_playback() stops the radio that just
         * started (TTS has priority), turning "play radio" into a song reply. */
        set_announce(announce, announce_size, "好的，正在为你播放电台");
        return VOICE_IOT_COMMAND_RADIO_PLAY;
    }
    if (contains_any(transcript, radio_pause_phrases, sizeof(radio_pause_phrases) / sizeof(radio_pause_phrases[0]))) {
        const esp_err_t err = radio_stream_service_pause();
        if (err == ESP_OK) set_announce(announce, announce_size, "好的，电台已暂停");
        return err == ESP_OK ? VOICE_IOT_COMMAND_RADIO_HANDLED : VOICE_IOT_COMMAND_FAILED;
    }
    if (contains_any(transcript, radio_stop_phrases, sizeof(radio_stop_phrases) / sizeof(radio_stop_phrases[0]))) {
        const esp_err_t err = radio_stream_service_stop();
        if (err == ESP_OK) set_announce(announce, announce_size, "好的，电台已停止");
        return err == ESP_OK ? VOICE_IOT_COMMAND_RADIO_HANDLED : VOICE_IOT_COMMAND_FAILED;
    }
    /* 兜底：云端 ASR 把"关闭电台"吞成"电台。"（实测），动词没了，短语表全部
       落空，云端模型只好自己挑一个动词——它挑的是"打开"。这里只认最保守的
       一种情况：整句话除了"电台"和标点之外别无他词，而且电台确实在播
       （is_requested 含让位阶段：打断电台说话时拉流已停，is_active 为假），
       才当作停止。 */
    if (radio_keyword_only(transcript) && radio_stream_service_is_requested()) {
        const esp_err_t err = radio_stream_service_stop();
        if (err == ESP_OK) set_announce(announce, announce_size, "好的，电台已停止");
        return err == ESP_OK ? VOICE_IOT_COMMAND_RADIO_HANDLED : VOICE_IOT_COMMAND_FAILED;
    }

    /* ------------------------------- Timers ------------------------------ */
    /* The phrase tables live at the top of this function: the stand-down check
     * above reads them before the local shortcuts may run. */

    if (contains_any(transcript, stop_ring_phrases,
                     sizeof(stop_ring_phrases) / sizeof(stop_ring_phrases[0]))) {
        if (!timer_service_is_ringing()) return VOICE_IOT_COMMAND_NOT_HANDLED;
        timer_service_stop_ringing();
        set_announce(announce, announce_size, "好的，闹钟关掉了");
        return VOICE_IOT_COMMAND_EXECUTED;
    }
    /* "取消10点的闹钟" must be intercepted before the set-alarm branch:
     * the set branch matches any text containing 闹钟 + a clock and would
     * otherwise create a second alarm.  The verb and 闹钟 may be separated
     * by the clock text, so match the verb and the keyword independently. */
    {
        static const char *const cancel_alarm_verbs[] = {
            "取消", "删除", "撤销", "去掉", "移除",
        };
        if (contains_any(transcript, cancel_alarm_verbs,
                         sizeof(cancel_alarm_verbs) / sizeof(cancel_alarm_verbs[0])) &&
            strstr(transcript, "闹钟") != NULL) {
            uint8_t hour = 0U;
            uint8_t minute = 0U;
            if (!find_clock_in_text(transcript, &hour, &minute)) {
                return VOICE_IOT_COMMAND_NOT_HANDLED;
            }
            if (timer_service_cancel_alarm(hour, minute) != TIMER_SERVICE_CMD_EXECUTED) {
                set_announce(announce, announce_size, "没有找到%u点%02u分的闹钟", hour, minute);
                return VOICE_IOT_COMMAND_EXECUTED;
            }
            set_announce(announce, announce_size, "好的，已取消%u点%02u分的闹钟", hour, minute);
            return VOICE_IOT_COMMAND_EXECUTED;
        }
    }
    if (contains_any(transcript, query_countdown_phrases,
                     sizeof(query_countdown_phrases) / sizeof(query_countdown_phrases[0]))) {
        uint32_t remaining = 0U;
        if (!timer_service_query_countdown_remaining(&remaining)) {
            set_announce(announce, announce_size, "现在没有正在计时的倒计时");
            return VOICE_IOT_COMMAND_EXECUTED;
        }
        const uint32_t minutes = remaining / 60U;
        const uint32_t seconds = remaining % 60U;
        if (minutes > 0U) {
            set_announce(announce, announce_size, "倒计时还剩%u分%u秒", minutes, seconds);
        } else {
            set_announce(announce, announce_size, "倒计时还剩%u秒", seconds);
        }
        return VOICE_IOT_COMMAND_EXECUTED;
    }
    if (contains_any(transcript, query_stopwatch_phrases,
                     sizeof(query_stopwatch_phrases) / sizeof(query_stopwatch_phrases[0]))) {
        uint32_t elapsed = 0U;
        (void)timer_service_query_stopwatch(&elapsed);
        const uint32_t minutes = elapsed / 60U;
        const uint32_t seconds = elapsed % 60U;
        if (minutes > 0U) {
            set_announce(announce, announce_size, "已经计时%u分%u秒", minutes, seconds);
        } else {
            set_announce(announce, announce_size, "已经计时%u秒", seconds);
        }
        return VOICE_IOT_COMMAND_EXECUTED;
    }
    if (contains_any(transcript, stop_stopwatch_phrases,
                     sizeof(stop_stopwatch_phrases) / sizeof(stop_stopwatch_phrases[0]))) {
        timer_service_stop_stopwatch();
        set_announce(announce, announce_size, "计时已停止");
        return VOICE_IOT_COMMAND_EXECUTED;
    }
    if (contains_any(transcript, pause_stopwatch_phrases,
                     sizeof(pause_stopwatch_phrases) / sizeof(pause_stopwatch_phrases[0]))) {
        if (timer_service_pause_stopwatch() != TIMER_SERVICE_CMD_EXECUTED) {
            return VOICE_IOT_COMMAND_NOT_HANDLED;
        }
        set_announce(announce, announce_size, "计时已暂停");
        return VOICE_IOT_COMMAND_EXECUTED;
    }
    if (contains_any(transcript, start_stopwatch_phrases,
                     sizeof(start_stopwatch_phrases) / sizeof(start_stopwatch_phrases[0]))) {
        if (timer_service_start_stopwatch() != TIMER_SERVICE_CMD_EXECUTED) {
            return VOICE_IOT_COMMAND_FAILED;
        }
        set_announce(announce, announce_size, "好的，开始计时");
        return VOICE_IOT_COMMAND_EXECUTED;
    }
    if (contains_any(transcript, countdown_phrases,
                     sizeof(countdown_phrases) / sizeof(countdown_phrases[0]))) {
        uint32_t seconds = 0U;
        if (!parse_duration_seconds(transcript, &seconds)) return VOICE_IOT_COMMAND_FAILED;
        if (timer_service_prepare_countdown(seconds) != TIMER_SERVICE_CMD_EXECUTED) {
            return VOICE_IOT_COMMAND_FAILED;
        }
        const uint32_t minutes = seconds / 60U;
        if (minutes > 0U && seconds % 60U == 0U) {
            set_announce(announce, announce_size, "好的，倒计时%u分钟", minutes);
        } else {
            set_announce(announce, announce_size, "好的，倒计时%u秒", seconds);
        }
        return VOICE_IOT_COMMAND_COUNTDOWN_PENDING;
    }
    if (contains_any(transcript, alarm_phrases,
                     sizeof(alarm_phrases) / sizeof(alarm_phrases[0]))) {
        uint8_t hour = 0U;
        uint8_t minute = 0U;
        if (!find_clock_in_text(transcript, &hour, &minute)) {
            return VOICE_IOT_COMMAND_FAILED;
        }
        /* 只有一块闹钟槽位，设新的会顶掉旧的。先取一份旧值，好在回答里说明
           “已取消上一个几点的闹钟”。云端路径（mcp_service 的 set_alarm）做同样的事，
           只是它把这句话放进工具返回值里让云端说出来。 */
        timer_service_snapshot_t previous;
        timer_service_get_snapshot(&previous);
        if (timer_service_set_alarm(hour, minute) != TIMER_SERVICE_CMD_EXECUTED) {
            return VOICE_IOT_COMMAND_FAILED;
        }
        if (previous.alarm_active) {
            set_announce(announce, announce_size, "好的，闹钟设在%u点%02u分，已取消上一个%u点的闹钟",
                         hour, minute, (unsigned)previous.alarm_hour);
        } else {
            set_announce(announce, announce_size, "好的，闹钟设在%u点%02u分", hour, minute);
        }
        return VOICE_IOT_COMMAND_EXECUTED;
    }
    return VOICE_IOT_COMMAND_NOT_HANDLED;
}

static voice_iot_command_result_t apply_local_volume(const char *transcript, float target)
{
    const float before = i2s_audio_driver_get_output_volume();
    const esp_err_t err = i2s_audio_driver_set_output_volume(target);
    ESP_LOGI("voice_iot", "volume command: text=%s before=%.0f target=%.0f err=%s",
             transcript, (double)before, (double)target, esp_err_to_name(err));
    if (err != ESP_OK) {
        return VOICE_IOT_COMMAND_FAILED;
    }
    /* Survives a power cycle; repeated requests are coalesced by the settings
     * service so a volume sweep still ends in a single NVS write. */
    device_settings_service_request_volume_save((int)(target + 0.5f));
    return VOICE_IOT_COMMAND_EXECUTED;
}
