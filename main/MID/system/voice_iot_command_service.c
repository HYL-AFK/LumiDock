#include "voice_iot_command_service.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "i2s_audio_driver.h"
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
    static const char *const radio_play_phrases[] = {"放电台", "播放电台", "打开电台"};
    static const char *const radio_pause_phrases[] = {"暂停电台", "暂停播放"};
    static const char *const radio_stop_phrases[] = {"停止电台", "关闭电台"};

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
    if (contains_any(transcript, volume_up_phrases, sizeof(volume_up_phrases) / sizeof(volume_up_phrases[0]))) {
        const float before = i2s_audio_driver_get_output_volume();
        const float after = before + 10.0f > 100.0f ? 100.0f : before + 10.0f;
        const esp_err_t err = i2s_audio_driver_set_output_volume(after);
        ESP_LOGI("voice_iot", "volume command: text=%s before=%.0f after=%.0f err=%s",
                 transcript, (double)before, (double)after, esp_err_to_name(err));
        return err == ESP_OK ? VOICE_IOT_COMMAND_EXECUTED : VOICE_IOT_COMMAND_FAILED;
    }
    if (contains_any(transcript, volume_down_phrases, sizeof(volume_down_phrases) / sizeof(volume_down_phrases[0]))) {
        const float before = i2s_audio_driver_get_output_volume();
        const float after = before - 10.0f < 0.0f ? 0.0f : before - 10.0f;
        const esp_err_t err = i2s_audio_driver_set_output_volume(after);
        ESP_LOGI("voice_iot", "volume command: text=%s before=%.0f after=%.0f err=%s",
                 transcript, (double)before, (double)after, esp_err_to_name(err));
        return err == ESP_OK ? VOICE_IOT_COMMAND_EXECUTED : VOICE_IOT_COMMAND_FAILED;
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
        return err == ESP_OK ? VOICE_IOT_COMMAND_EXECUTED : VOICE_IOT_COMMAND_FAILED;
    }
    if (contains_any(transcript, radio_stop_phrases, sizeof(radio_stop_phrases) / sizeof(radio_stop_phrases[0]))) {
        const esp_err_t err = radio_stream_service_stop();
        if (err == ESP_OK) set_announce(announce, announce_size, "好的，电台已停止");
        return err == ESP_OK ? VOICE_IOT_COMMAND_EXECUTED : VOICE_IOT_COMMAND_FAILED;
    }

    /* ------------------------------- Timers ------------------------------ */

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
        if (timer_service_set_alarm(hour, minute) != TIMER_SERVICE_CMD_EXECUTED) {
            return VOICE_IOT_COMMAND_FAILED;
        }
        set_announce(announce, announce_size, "好的，闹钟设在%u点%02u分", hour, minute);
        return VOICE_IOT_COMMAND_EXECUTED;
    }
    return VOICE_IOT_COMMAND_NOT_HANDLED;
}
