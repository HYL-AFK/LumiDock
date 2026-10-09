#include "mcp_service.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "charge_strategy.h"
#include "device_settings_service.h"
#include "i2s_audio_driver.h"
#include "news_service.h"
#include "radio_stream_service.h"
#include "timer_service.h"

static const char *const TAG = "mcp";

/* The tools/list reply is the widest frame we ever build (every tool carries
 * its full description), and the catalogue is already pressed right against the
 * old 4096 ceiling).  So the
 * buffer moved out of .bss into PSRAM and grew: internal RAM is the scarce
 * resource here, PSRAM is not, and this hands the常驻 4 KB back.
 *
 * 取 6144 而不是更大：当初是照着"工具目录再加一截响应信封"定的，目录本身仍在
 * flash 里，缓冲区只在第一次发回复时分配，没必要为留空而白吃 PSRAM。
 * 目录放不放得下由下面的 _Static_assert 在编译期把关——加工具越界时构建直接
 * 失败，而不是等云端收到半截 JSON、把设备控制整个静默关掉才发现。
 * 语音服务的信封缓冲 (XIAOZHI_MCP_FRAME_MAX_BYTES) 必须比它大。 */
#define MCP_REPLY_MAX_BYTES 6144U

#define MCP_PROTOCOL_VERSION "2024-11-05"
#define MCP_SERVER_NAME "LumiDock"
#define MCP_SERVER_VERSION "1.0.0"

/* Tool names follow the upstream xiaozhi server contract: the cloud prompt is
 * already written against the volume and status names, so renaming those two
 * breaks device control even though the transport would still work.  The timer
 * and radio names are ours and follow the same dotted convention. */
#define MCP_TOOL_GET_STATUS "self.get_device_status"
#define MCP_TOOL_SET_VOLUME "self.audio_speaker.set_volume"
#define MCP_TOOL_SET_ALARM "self.timer.set_alarm"
#define MCP_TOOL_CANCEL_ALARM "self.timer.cancel_alarm"
#define MCP_TOOL_SET_COUNTDOWN "self.timer.set_countdown"
#define MCP_TOOL_START_STOPWATCH "self.timer.start_stopwatch"
#define MCP_TOOL_PAUSE_STOPWATCH "self.timer.pause_stopwatch"
#define MCP_TOOL_STOP_STOPWATCH "self.timer.stop_stopwatch"
#define MCP_TOOL_STOP_RINGING "self.timer.stop_ringing"
#define MCP_TOOL_RADIO_PLAY "self.radio.play"
#define MCP_TOOL_RADIO_NEXT "self.radio.next"
#define MCP_TOOL_RADIO_PAUSE "self.radio.pause"
#define MCP_TOOL_RADIO_RESUME "self.radio.resume"
#define MCP_TOOL_RADIO_STOP "self.radio.stop"
#define MCP_TOOL_CHARGE_PLAN "self.charge.get_plan"
#define MCP_TOOL_NEWS_FETCH "self.news.fetch"

/* JSON-RPC 2.0 error codes. */
#define MCP_ERROR_METHOD_NOT_FOUND (-32601)
#define MCP_ERROR_INVALID_PARAMS (-32602)
#define MCP_ERROR_INTERNAL (-32603)

/* The tool catalogue is a compile-time constant: it never changes at run time,
 * so building it with cJSON on every tools/list request would only add heap
 * churn and a way to truncate the answer.  Keeping it static also means the
 * description text lives in flash instead of RAM. */
static const char k_tools_list_result_json[] =
    "{\"tools\":["
    "{\"name\":\"" MCP_TOOL_GET_STATUS "\",\"description\":\""
    "Provides the real-time information of the device: the audio speaker, the timers "
    "(alarm, countdown, stopwatch) and the radio. Use this tool to answer questions "
    "about the current condition and as the first step before controlling the device."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"" MCP_TOOL_SET_VOLUME "\",\"description\":\""
    "Set the speaker volume to an absolute level 0-100. Always call "
    "self.get_device_status first: the value is applied verbatim, so a guess is heard at once. "
    "Use this only when the user names a level. Relative requests such as turning it up or "
    "asking for less volume are handled by the device itself and must not be forwarded here."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"volume\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},"
    "\"required\":[\"volume\"]}},"
    "{\"name\":\"" MCP_TOOL_SET_ALARM "\",\"description\":\""
    "Set a one-shot alarm that rings at the next occurrence of hour:minute local time "
    "(tomorrow when that moment already passed). The device clock must be synchronized. "
    "hour is 0-23 and minute is 0-59. There is a single alarm slot, so a new alarm "
    "replaces the pending one. The result names the alarm that was replaced, and "
    "you must tell the user which alarm was cancelled."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"hour\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":23},"
    "\"minute\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":59}},"
    "\"required\":[\"hour\",\"minute\"]}},"
    "{\"name\":\"" MCP_TOOL_CANCEL_ALARM "\",\"description\":\""
    "Cancel the pending alarm that would ring at hour:minute local time. hour is 0-23 "
    "and minute is 0-59."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"hour\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":23},"
    "\"minute\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":59}},"
    "\"required\":[\"hour\",\"minute\"]}},"
    "{\"name\":\"" MCP_TOOL_SET_COUNTDOWN "\",\"description\":\""
    "Start a countdown that rings after the given number of seconds. Only one "
    "countdown exists at a time: calling this again replaces the pending one, and "
    "it cannot run while the stopwatch is running."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"seconds\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":3599}},"
    "\"required\":[\"seconds\"]}},"
    "{\"name\":\"" MCP_TOOL_START_STOPWATCH "\",\"description\":\""
    "Start or resume the stopwatch. It cannot run while a countdown is active."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"" MCP_TOOL_PAUSE_STOPWATCH "\",\"description\":\""
    "Pause the running stopwatch and keep the elapsed time."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"" MCP_TOOL_STOP_STOPWATCH "\",\"description\":\""
    "Stop the stopwatch and clear the elapsed time."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"" MCP_TOOL_STOP_RINGING "\",\"description\":\""
    "Silence the alarm or countdown that is ringing right now."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"" MCP_TOOL_RADIO_PLAY "\",\"description\":\""
    "Start the built-in live radio. There is no channel argument: the device owns "
    "the channel list and starts on its own channel, so never promise a particular "
    "channel by name. When the user asks for a different channel (换一个台), call "
    MCP_TOOL_RADIO_NEXT "."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"" MCP_TOOL_RADIO_NEXT "\",\"description\":\""
    "Switch the radio to a different channel, moving one step and wrapping around at "
    "the end. Call this whenever the user asks for another channel (换一个台 / 换台). "
    "A channel name is never accepted: the device owns the channel list and only "
    "ever moves one step, so never promise a particular channel."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"" MCP_TOOL_RADIO_PAUSE "\",\"description\":\""
    "Pause the radio stream. Use self.radio.resume to continue it."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"" MCP_TOOL_RADIO_RESUME "\",\"description\":\""
    "Resume the paused radio stream."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"" MCP_TOOL_RADIO_STOP "\",\"description\":\""
    "Stop the radio stream and release the audio channel."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"" MCP_TOOL_CHARGE_PLAN "\",\"description\":\""
    "Decide whether the e-bike needs charging and when to do it. Call this for any "
    "question about charging, battery level, remaining range or tomorrow's trip. It "
    "reads the battery, tomorrow's weather and the hourly electricity price, then "
    "returns a ready-to-speak sentence in reason: read that sentence to the user "
    "verbatim and do not invent a plan of your own. The device has no odometer, so "
    "the daily distance is a fixed configuration value."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"" MCP_TOOL_NEWS_FETCH "\",\"description\":\""
    "Read today's headlines aloud. Use it whenever the user asks for news, headlines, "
    "or what is happening. category is one of news, economy, arts or entertainment "
    "and defaults to news; count is how many headlines to read, 1 to 10, default 5. "
    "The reply lists the headlines in items: read them in order, and do not summarize "
    "them or add commentary of your own."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"category\":{\"type\":\"string\",\"description\":\"news, economy, arts or entertainment\"},"
    "\"count\":{\"type\":\"integer\",\"description\":\"how many headlines to read, 1 to 10\"}}}}"
    "]}";

/* A truncated tools/list answer silently disables every device control, so the
 * catalogue is verified against the reply buffer at compile time. */
_Static_assert(sizeof(k_tools_list_result_json) + 64U <= MCP_REPLY_MAX_BYTES,
               "MCP tool catalogue must fit the reply buffer");

/* 回复缓冲搬到 PSRAM。工具目录本身仍在 flash（k_tools_list_result_json），
 * 这里只装 JSON-RPC 响应帧。惰性分配：第一次要发回复时拿一块，之后一直复用，
 * 避免每帧 malloc/free 抖动。 */
static char *s_reply;
static size_t s_reply_capacity;

static bool ensure_reply_buffer(void)
{
    if (s_reply != NULL)
        return true;
    /* 优先 PSRAM：内部 RAM 要留给 WiFi / AFE / 音频链路。拿不到再退回内部 RAM，
       那样只是没省下那 4 KB，功能不受影响。 */
    s_reply = heap_caps_malloc(MCP_REPLY_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_reply == NULL)
        s_reply = heap_caps_malloc(MCP_REPLY_MAX_BYTES, MALLOC_CAP_8BIT);
    if (s_reply == NULL)
    {
        ESP_LOGE(TAG, "MCP reply buffer allocation failed (%u bytes)",
                 (unsigned)MCP_REPLY_MAX_BYTES);
        return false;
    }
    s_reply_capacity = MCP_REPLY_MAX_BYTES;
    return true;
}

/* Set once the cloud has seen the tool list; the local command shortcuts read
 * it through mcp_service_is_ready() to avoid applying a change twice. */
static bool s_discovery_done;

/* Deferred starts requested by a tool call (see mcp_service.h).  Both are only
 * touched from the voice session task, so no lock is needed. */
static volatile bool s_pending_countdown;
static volatile bool s_pending_radio;
/* 这次待启动的是"下一个频道"还是"默认台"。云端只说"用户要动电台"，频道表在
 * 设备手里，所以到底切哪个由这里记下的意图决定。 */
static volatile bool s_pending_radio_next;
/* Set once a local radio command has been applied (see mcp_service.h): the
 * cloud keeps talking about the same utterance, and its radio tools must not
 * be allowed to undo what already happened. */
static volatile bool s_radio_tools_blocked;

/* ------------------------------------------------------------------ */
/* JSON-RPC response builders                                          */
/* ------------------------------------------------------------------ */

static const char *reply_result_raw(long id, const char *result_json)
{
    if (!ensure_reply_buffer())
        return NULL;
    const int written = snprintf(s_reply, s_reply_capacity,
                                 "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":%s}", id,
                                 result_json);
    if (written <= 0 || (size_t)written >= s_reply_capacity)
    {
        ESP_LOGE(TAG, "MCP result reply dropped: %d bytes needed", written);
        return NULL;
    }
    return s_reply;
}

static const char *reply_error(long id, int code, const char *message)
{
    if (!ensure_reply_buffer())
        return NULL;
    const int written = snprintf(s_reply, s_reply_capacity,
                                 "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
                                 id, code, message);
    if (written <= 0 || (size_t)written >= s_reply_capacity)
    {
        ESP_LOGE(TAG, "MCP error reply dropped: %d bytes needed", written);
        return NULL;
    }
    return s_reply;
}

/* Wraps a tool return value the way the upstream server does: the text content
 * carries the tool result, either "true" or a serialised JSON document. */
static const char *reply_tool_text(long id, const char *json_escaped_text)
{
    if (!ensure_reply_buffer())
        return NULL;
    const int written = snprintf(
        s_reply, s_reply_capacity,
        "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],\"isError\":false}}",
        id, json_escaped_text);
    if (written <= 0 || (size_t)written >= s_reply_capacity)
    {
        ESP_LOGE(TAG, "MCP tool reply dropped: %d bytes needed", written);
        return NULL;
    }
    return s_reply;
}

/* ------------------------------------------------------------------ */
/* Tools                                                               */
/* ------------------------------------------------------------------ */

/* A tool result travels inside a JSON string, so the document has to be
 * escaped before it reaches reply_tool_text(). */
static bool json_escape_into(char *dst, size_t dst_size, const char *src)
{
    size_t used = 0U;
    for (const char *p = src; *p != '\0'; ++p)
    {
        const char *replacement = NULL;
        switch (*p)
        {
        case '"': replacement = "\\\""; break;
        case '\\': replacement = "\\\\"; break;
        case '\n': replacement = "\\n"; break;
        case '\r': replacement = "\\r"; break;
        case '\t': replacement = "\\t"; break;
        default: break;
        }
        if (replacement == NULL)
        {
            if (used + 2U > dst_size) return false;
            dst[used++] = *p;
            continue;
        }
        const size_t length = strlen(replacement);
        if (used + length + 1U > dst_size) return false;
        memcpy(dst + used, replacement, length);
        used += length;
    }
    dst[used] = '\0';
    return true;
}

static const char *handle_get_device_status(long id)
{
    const int volume = (int)(i2s_audio_driver_get_output_volume() + 0.5f);
    timer_service_snapshot_t timers;
    timer_service_get_snapshot(&timers);
    /* 用 is_requested() 而不是 is_active()：语音让位期间电台会停掉拉流，
       is_active() 会短暂变假，但用户的意图仍是“电台在播”，上报不能因此翻成
       没在播，否则云端会重新起一个。 */
    const bool radio_playing = radio_stream_service_is_requested();
    /* 当前频道名。云端靠它把“换个台”算成具体的下一个名字 —— 没有这个字段，
       “换个台”就无从下手（它只能看到一个布尔值）。 */
    const char *radio_station = radio_stream_service_station_name();
    char plain[512];
    const int written = snprintf(
        plain, sizeof(plain),
        "{\"audio_speaker\":{\"volume\":%d},"
        "\"timer\":{\"alarm_active\":%s,\"alarm_time\":\"%02u:%02u\","
        "\"countdown_active\":%s,\"countdown_remaining_seconds\":%lu,"
        "\"stopwatch_running\":%s,\"stopwatch_seconds\":%lu,\"ringing\":%s},"
        "\"radio\":{\"playing\":%s,\"station\":\"%s\"}}",
        volume, timers.alarm_active ? "true" : "false", (unsigned)timers.alarm_hour,
        (unsigned)timers.alarm_minute, timers.countdown_active ? "true" : "false",
        (unsigned long)timers.countdown_remaining_seconds,
        timers.stopwatch_running ? "true" : "false",
        (unsigned long)timers.stopwatch_seconds, timers.ringing ? "true" : "false",
        radio_playing ? "true" : "false", radio_station);
    if (written <= 0 || (size_t)written >= sizeof(plain))
    {
        return reply_error(id, MCP_ERROR_INTERNAL, "Failed to build the device status");
    }
    char escaped[700];
    if (!json_escape_into(escaped, sizeof(escaped), plain))
    {
        return reply_error(id, MCP_ERROR_INTERNAL, "Failed to encode the device status");
    }
    ESP_LOGI(TAG,
             "tool call: %s volume=%d alarm=%d countdown=%d stopwatch=%d ringing=%d radio=%d",
             MCP_TOOL_GET_STATUS, volume, timers.alarm_active ? 1 : 0,
             timers.countdown_active ? 1 : 0, timers.stopwatch_running ? 1 : 0,
             timers.ringing ? 1 : 0, radio_playing ? 1 : 0);
    return reply_tool_text(id, escaped);
}

static const char *handle_set_volume(const cJSON *arguments, long id)
{
    const cJSON *volume =
        cJSON_IsObject(arguments) ? cJSON_GetObjectItemCaseSensitive(arguments, "volume") : NULL;
    if (!cJSON_IsNumber(volume))
    {
        ESP_LOGW(TAG, "tool call rejected: %s without a numeric volume", MCP_TOOL_SET_VOLUME);
        return reply_error(id, MCP_ERROR_INVALID_PARAMS, "Missing valid argument: volume");
    }
    /* Clamp instead of failing: the model occasionally overshoots in its own
     * summary ("turn it all the way up" arriving as 120) and refusing there
     * would surface as another "cannot do that" reply. */
    int target = volume->valueint;
    if (target < 0)
    {
        target = 0;
    }
    else if (target > 100)
    {
        target = 100;
    }
    const float before = i2s_audio_driver_get_output_volume();
    const esp_err_t err = i2s_audio_driver_set_output_volume((float)target);
    ESP_LOGI(TAG, "tool call: %s before=%.0f target=%d err=%s", MCP_TOOL_SET_VOLUME,
             (double)before, target, esp_err_to_name(err));
    if (err != ESP_OK)
    {
        return reply_error(id, MCP_ERROR_INTERNAL, "Failed to set the speaker volume");
    }
    /* The user expects the level to survive a reboot. */
    device_settings_service_request_volume_save(target);
    return reply_tool_text(id, "true");
}

static const char *handle_set_alarm(const cJSON *arguments, long id)
{
    const cJSON *hour =
        cJSON_IsObject(arguments) ? cJSON_GetObjectItemCaseSensitive(arguments, "hour") : NULL;
    const cJSON *minute =
        cJSON_IsObject(arguments) ? cJSON_GetObjectItemCaseSensitive(arguments, "minute") : NULL;
    if (!cJSON_IsNumber(hour) || !cJSON_IsNumber(minute))
    {
        ESP_LOGW(TAG, "tool call rejected: %s without a numeric hour/minute", MCP_TOOL_SET_ALARM);
        return reply_error(id, MCP_ERROR_INVALID_PARAMS, "Missing valid arguments: hour, minute");
    }
    const int hour_value = hour->valueint;
    const int minute_value = minute->valueint;
    if (hour_value < 0 || hour_value > 23 || minute_value < 0 || minute_value > 59)
    {
        ESP_LOGW(TAG, "tool call rejected: %s out of range %02d:%02d", MCP_TOOL_SET_ALARM,
                 hour_value, minute_value);
        return reply_error(id, MCP_ERROR_INVALID_PARAMS, "hour must be 0-23 and minute must be 0-59");
    }
    /* An alarm is meaningless without the wall clock; the local shortcut
     * rejects it the same way so both paths answer identically. */
    if (!timer_service_is_time_synced())
    {
        ESP_LOGW(TAG, "tool call rejected: %s before the clock was synchronized", MCP_TOOL_SET_ALARM);
        return reply_error(id, MCP_ERROR_INTERNAL, "The device clock is not synchronized yet");
    }
    /* 只有一块闹钟槽位，设新的会顶掉旧的。先取一份旧值，好让云端把
       “已取消上一个几点的闹钟” 说给用户听——设备自己说不出动态文本。 */
    timer_service_snapshot_t previous;
    timer_service_get_snapshot(&previous);
    const timer_service_cmd_result_t result =
        timer_service_set_alarm((uint8_t)hour_value, (uint8_t)minute_value);
    ESP_LOGI(TAG, "tool call: %s time=%02d:%02d result=%d replaced=%d", MCP_TOOL_SET_ALARM,
             hour_value, minute_value, (int)result, previous.alarm_active ? 1 : 0);
    if (result != TIMER_SERVICE_CMD_EXECUTED)
    {
        return reply_error(id, MCP_ERROR_INTERNAL, "Failed to set the alarm");
    }
    /* 返回值是模型唯一能看到的执行报告。带上被顶掉的那个时间，再配合工具描述里的
       硬要求，模型才会把“取消了上一个几点的”讲出来。 */
    if (previous.alarm_active)
    {
        char report[192];
        const int written = snprintf(
            report, sizeof(report),
            "Alarm set for %02d:%02d. The previous alarm at %02d:%02d was cancelled, "
            "so only the new one is pending. Tell the user which alarm was replaced.",
            hour_value, minute_value, (int)previous.alarm_hour, (int)previous.alarm_minute);
        if (written > 0 && (size_t)written < sizeof(report))
        {
            return reply_tool_text(id, report);
        }
    }
    return reply_tool_text(id, "true");
}

static const char *handle_cancel_alarm(const cJSON *arguments, long id)
{
    const cJSON *hour =
        cJSON_IsObject(arguments) ? cJSON_GetObjectItemCaseSensitive(arguments, "hour") : NULL;
    const cJSON *minute =
        cJSON_IsObject(arguments) ? cJSON_GetObjectItemCaseSensitive(arguments, "minute") : NULL;
    if (!cJSON_IsNumber(hour) || !cJSON_IsNumber(minute))
    {
        ESP_LOGW(TAG, "tool call rejected: %s without a numeric hour/minute", MCP_TOOL_CANCEL_ALARM);
        return reply_error(id, MCP_ERROR_INVALID_PARAMS, "Missing valid arguments: hour, minute");
    }
    const int hour_value = hour->valueint;
    const int minute_value = minute->valueint;
    if (hour_value < 0 || hour_value > 23 || minute_value < 0 || minute_value > 59)
    {
        ESP_LOGW(TAG, "tool call rejected: %s out of range %02d:%02d", MCP_TOOL_CANCEL_ALARM,
                 hour_value, minute_value);
        return reply_error(id, MCP_ERROR_INVALID_PARAMS, "hour must be 0-23 and minute must be 0-59");
    }
    const timer_service_cmd_result_t result =
        timer_service_cancel_alarm((uint8_t)hour_value, (uint8_t)minute_value);
    ESP_LOGI(TAG, "tool call: %s time=%02d:%02d result=%d", MCP_TOOL_CANCEL_ALARM, hour_value,
             minute_value, (int)result);
    if (result != TIMER_SERVICE_CMD_EXECUTED)
    {
        return reply_error(id, MCP_ERROR_INTERNAL, "No pending alarm matches that time");
    }
    return reply_tool_text(id, "true");
}

static const char *handle_set_countdown(const cJSON *arguments, long id)
{
    const cJSON *seconds =
        cJSON_IsObject(arguments) ? cJSON_GetObjectItemCaseSensitive(arguments, "seconds") : NULL;
    if (!cJSON_IsNumber(seconds))
    {
        ESP_LOGW(TAG, "tool call rejected: %s without a numeric seconds", MCP_TOOL_SET_COUNTDOWN);
        return reply_error(id, MCP_ERROR_INVALID_PARAMS, "Missing valid argument: seconds");
    }
    const int value = seconds->valueint;
    if (value <= 0 || (uint32_t)value > TIMER_SERVICE_MAX_DISPLAY_SECONDS)
    {
        ESP_LOGW(TAG, "tool call rejected: %s out of range %d", MCP_TOOL_SET_COUNTDOWN, value);
        return reply_error(id, MCP_ERROR_INVALID_PARAMS, "seconds must be between 1 and 3599");
    }
    /* Reserve the slot and let the voice service start it once the
     * acknowledgement TTS drained, so the countdown matches the spoken answer
     * instead of ticking underneath it. */
    const timer_service_cmd_result_t result = timer_service_prepare_countdown((uint32_t)value);
    ESP_LOGI(TAG, "tool call: %s seconds=%d result=%d", MCP_TOOL_SET_COUNTDOWN, value, (int)result);
    if (result != TIMER_SERVICE_CMD_EXECUTED)
    {
        return reply_error(id, MCP_ERROR_INTERNAL, "Failed to start the countdown");
    }
    s_pending_countdown = true;
    return reply_tool_text(id, "true");
}

static const char *handle_stopwatch(const char *tool, timer_service_cmd_result_t result, long id)
{
    ESP_LOGI(TAG, "tool call: %s result=%d", tool, (int)result);
    if (result != TIMER_SERVICE_CMD_EXECUTED)
    {
        return reply_error(id, MCP_ERROR_INTERNAL, "The stopwatch command was rejected");
    }
    return reply_tool_text(id, "true");
}

static const char *handle_stop_ringing(long id)
{
    const bool was_ringing = timer_service_is_ringing();
    timer_service_stop_ringing();
    ESP_LOGI(TAG, "tool call: %s ringing=%d", MCP_TOOL_STOP_RINGING, was_ringing ? 1 : 0);
    /* Idempotent on purpose: a repeated request must not turn into a failure
     * the model would then apologise for. */
    return reply_tool_text(id, "true");
}

/* 播放和换台是同一条延迟路径的两种意图：云端都会用语音应答这次调用，而那段 TTS
 * 占着共享播放通道，当场起流只会被它静音。差别只有"从哪个频道开始"，一个标志带过去。 */
static const char *handle_radio_start(bool next, long id)
{
    s_pending_radio = true;
    s_pending_radio_next = next;
    ESP_LOGI(TAG, "tool call: %s deferred until the acknowledgement TTS drains",
             next ? MCP_TOOL_RADIO_NEXT : MCP_TOOL_RADIO_PLAY);
    return reply_tool_text(id, "true");
}

static const char *handle_radio_command(const char *tool, esp_err_t err, long id)
{
    ESP_LOGI(TAG, "tool call: %s err=%s", tool, esp_err_to_name(err));
    if (err != ESP_OK)
    {
        return reply_error(id, MCP_ERROR_INTERNAL, "The radio command failed");
    }
    return reply_tool_text(id, "true");
}

/* 新闻回包比充电建议长得多：10 条标题本身就 1270 字节，再套一层转义会翻倍。
 * 摆到会话任务栈上会顶穿内部 RAM，所以整块挪进 PSRAM——和 MCP 回复缓冲同一个
 * 思路。惰性分配，谁先用到谁付这 4.8 KB。 */
#define NEWS_PLAIN_BYTES 1536U
#define NEWS_ESCAPED_BYTES 2048U

static news_digest_t *s_news_digest;
static char *s_news_plain;
static char *s_news_escaped;

static bool ensure_news_scratch(void)
{
    /* 三块分开检查：只判第一块的话，上一次分配中途失败会让这里直接放行，
       然后拿着 NULL 去 snprintf。 */
    if (s_news_digest == NULL)
    {
        s_news_digest = heap_caps_malloc(sizeof(news_digest_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_news_plain == NULL)
    {
        s_news_plain = heap_caps_malloc(NEWS_PLAIN_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_news_escaped == NULL)
    {
        s_news_escaped = heap_caps_malloc(NEWS_ESCAPED_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_news_digest == NULL || s_news_plain == NULL || s_news_escaped == NULL)
    {
        ESP_LOGE(TAG, "新闻回包缓冲分配失败");
        return false;
    }
    return true;
}

/* 分类名和对外名字共用一张表，避免两处字符串各写一份然后慢慢漂移。 */
static bool parse_news_category(const char *text, news_category_t *out)
{
    for (uint32_t i = 0U; i < NEWS_CATEGORY_COUNT; ++i)
    {
        if (strcmp(text, news_service_category_name((news_category_t)i)) == 0)
        {
            *out = (news_category_t)i;
            return true;
        }
    }
    return false;
}

static uint32_t clamp_news_count(const cJSON *arguments)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(arguments, "count");
    if (!cJSON_IsNumber(item))
    {
        return 5U;
    }
    const int raw = item->valueint;
    if (raw < 1)
    {
        return 1U;
    }
    if (raw > (int)NEWS_MAX_ITEMS)
    {
        return NEWS_MAX_ITEMS;
    }
    return (uint32_t)raw;
}

static const char *handle_news_fetch(const cJSON *arguments, long id)
{
    news_category_t category = NEWS_CATEGORY_NEWS;
    if (arguments != NULL)
    {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(arguments, "category");
        if (cJSON_IsString(item) && item->valuestring != NULL &&
            !parse_news_category(item->valuestring, &category))
        {
            return reply_error(id, MCP_ERROR_INVALID_PARAMS, "Unknown news category");
        }
    }
    if (!ensure_news_scratch())
    {
        return reply_error(id, MCP_ERROR_INTERNAL, "Out of memory for the news reply");
    }
    /* 还没拉到就说"还在加载"：这是正常等待不是故障，别让云端念一句报错。 */
    if (!news_service_get(category, s_news_digest))
    {
        return reply_tool_text(id, "新闻还在加载，过一会儿再问我一次。");
    }

    uint32_t want = clamp_news_count(arguments);
    if (want > s_news_digest->count)
    {
        want = s_news_digest->count;
    }
    int written = snprintf(s_news_plain, NEWS_PLAIN_BYTES,
                           "{\"category\":\"%s\",\"count\":%u,\"items\":[",
                           news_service_category_name(category), (unsigned)want);
    if (written <= 0)
    {
        return reply_error(id, MCP_ERROR_INTERNAL, "Failed to build the news reply");
    }
    for (uint32_t i = 0U; i < want; ++i)
    {
        const int room = (int)NEWS_PLAIN_BYTES - written;
        if (room <= 4)
        {
            written = (int)NEWS_PLAIN_BYTES;
            break;
        }
        const int added = snprintf(s_news_plain + written, (size_t)room, "%s\"%s\"",
                                   i == 0U ? "" : ",", s_news_digest->titles[i]);
        /* added >= room 说明被截断了，这时 written 已经不能当"已用长度"用。 */
        if (added <= 0 || added >= room)
        {
            written = (int)NEWS_PLAIN_BYTES;
            break;
        }
        written += added;
    }
    if ((size_t)written >= NEWS_PLAIN_BYTES - 3U)
    {
        return reply_error(id, MCP_ERROR_INTERNAL, "Failed to build the news reply");
    }
    written += snprintf(s_news_plain + written, NEWS_PLAIN_BYTES - (size_t)written, "]}");
    if (written <= 0 || (size_t)written >= NEWS_PLAIN_BYTES)
    {
        return reply_error(id, MCP_ERROR_INTERNAL, "Failed to build the news reply");
    }
    if (!json_escape_into(s_news_escaped, NEWS_ESCAPED_BYTES, s_news_plain))
    {
        return reply_error(id, MCP_ERROR_INTERNAL, "Failed to encode the news reply");
    }
    ESP_LOGI(TAG, "tool call: %s category=%s count=%u", MCP_TOOL_NEWS_FETCH,
             news_service_category_name(category), (unsigned)want);
    return reply_tool_text(id, s_news_escaped);
}

/* 充电建议整句由设备侧组织好，云端只负责念。让模型自己"组织语言"会把具体数字
 * 说错——电量、时长、省多少钱，这些它都看不到。整块放 PSRAM：reason 本身 384
 * 字节，套上 warning 再转义一层轻松过 1.2 KB，摆在会话任务栈上不合适。 */
#define CHARGE_PLAIN_BYTES 1024U
#define CHARGE_ESCAPED_BYTES 2048U

static charge_plan_t *s_charge_plan;
static char *s_charge_plain;
static char *s_charge_escaped;

static bool ensure_charge_scratch(void)
{
    if (s_charge_plan == NULL)
    {
        s_charge_plan = heap_caps_malloc(sizeof(charge_plan_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_charge_plain == NULL)
    {
        s_charge_plain = heap_caps_malloc(CHARGE_PLAIN_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_charge_escaped == NULL)
    {
        s_charge_escaped = heap_caps_malloc(CHARGE_ESCAPED_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_charge_plan == NULL || s_charge_plain == NULL || s_charge_escaped == NULL)
    {
        ESP_LOGE(TAG, "充电回包缓冲分配失败");
        return false;
    }
    return true;
}

static const char *handle_charge_get_plan(long id)
{
    if (!ensure_charge_scratch())
    {
        return reply_error(id, MCP_ERROR_INTERNAL, "Out of memory for the charge plan");
    }
    /* 读不到电池不是"工具出错"，而是一个用户能理解、也能自己处理的状态：报 error
     * 会让云端回一个 😔 加一句道歉，UI 上看着像设备故障。这里给一句能直接念的话，
     * 和新闻那边"还在加载"的处理保持一致。真正的内部故障（缓冲分配失败）仍然报 error。 */
    if (!charge_strategy_build_plan(s_charge_plan))
    {
        return reply_tool_text(id, "现在读不到电池数据，可能是电池还没接上，接好之后再问我一次。");
    }
    /* energy_kwh 走定点整数：格式串里只要出现浮点，-Wformat-truncation 就会按
     * DBL_MAX 估上界（约 310 字符），静态分析认定必然截断，-Werror 直接挂构建。 */
    const int energy_x100 = (int)(s_charge_plan->energy_kwh * 100.0f + 0.5f);
    const int written = snprintf(
        s_charge_plain, CHARGE_PLAIN_BYTES,
        "{\"action\":\"%s\",\"should_charge\":%s,\"reason\":\"%s\",\"warning\":\"%s\","
        "\"current_soc\":%u,\"target_soc\":%u,\"battery_temp_c\":%d,"
        "\"energy_kwh\":%d.%02d,\"charge_minutes\":%u}",
        charge_strategy_action_name(s_charge_plan->action),
        s_charge_plan->should_charge ? "true" : "false", s_charge_plan->reason,
        s_charge_plan->warning, (unsigned)s_charge_plan->current_soc,
        (unsigned)s_charge_plan->target_soc, (int)s_charge_plan->battery_temp_c,
        energy_x100 / 100, energy_x100 % 100, (unsigned)s_charge_plan->charge_minutes);
    if (written <= 0 || (size_t)written >= CHARGE_PLAIN_BYTES)
    {
        return reply_error(id, MCP_ERROR_INTERNAL, "Failed to build the charge plan");
    }
    if (!json_escape_into(s_charge_escaped, CHARGE_ESCAPED_BYTES, s_charge_plain))
    {
        return reply_error(id, MCP_ERROR_INTERNAL, "Failed to encode the charge plan");
    }
    ESP_LOGI(TAG, "tool call: %s action=%s soc=%u%%", MCP_TOOL_CHARGE_PLAN,
             charge_strategy_action_name(s_charge_plan->action),
             (unsigned)s_charge_plan->current_soc);
    return reply_tool_text(id, s_charge_escaped);
}

static bool is_radio_tool(const char *tool)
{
    return strcmp(tool, MCP_TOOL_RADIO_PLAY) == 0 ||
           strcmp(tool, MCP_TOOL_RADIO_NEXT) == 0 ||
           strcmp(tool, MCP_TOOL_RADIO_PAUSE) == 0 ||
           strcmp(tool, MCP_TOOL_RADIO_RESUME) == 0 ||
           strcmp(tool, MCP_TOOL_RADIO_STOP) == 0;
}

static const char *handle_tools_call(const cJSON *params, long id)
{
    if (!cJSON_IsObject(params))
    {
        ESP_LOGW(TAG, "tool call rejected: params missing");
        return reply_error(id, MCP_ERROR_INVALID_PARAMS, "Missing params");
    }
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (!cJSON_IsString(name) || name->valuestring == NULL)
    {
        ESP_LOGW(TAG, "tool call rejected: name missing");
        return reply_error(id, MCP_ERROR_INVALID_PARAMS, "Missing name");
    }
    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    if (arguments != NULL && !cJSON_IsObject(arguments))
    {
        ESP_LOGW(TAG, "tool call rejected: arguments is not an object");
        return reply_error(id, MCP_ERROR_INVALID_PARAMS, "Invalid arguments");
    }
    const char *const tool = name->valuestring;
    /* A local radio command already changed the station this turn.  The model
     * answered the same utterance, and a lossy transcript ("关闭电台" ->
     * "电台。") makes it pick the opposite verb, so its radio tools are muted
     * here.  "true" is the deliberate answer: an error would only make the
     * cloud retry the call. */
    if (s_radio_tools_blocked && is_radio_tool(tool))
    {
        ESP_LOGW(TAG, "tool call ignored: %s already handled locally", tool);
        return reply_tool_text(id, "true");
    }
    if (strcmp(tool, MCP_TOOL_SET_VOLUME) == 0)
    {
        return handle_set_volume(arguments, id);
    }
    if (strcmp(tool, MCP_TOOL_GET_STATUS) == 0)
    {
        return handle_get_device_status(id);
    }
    if (strcmp(tool, MCP_TOOL_SET_ALARM) == 0)
    {
        return handle_set_alarm(arguments, id);
    }
    if (strcmp(tool, MCP_TOOL_CANCEL_ALARM) == 0)
    {
        return handle_cancel_alarm(arguments, id);
    }
    if (strcmp(tool, MCP_TOOL_SET_COUNTDOWN) == 0)
    {
        return handle_set_countdown(arguments, id);
    }
    if (strcmp(tool, MCP_TOOL_START_STOPWATCH) == 0)
    {
        return handle_stopwatch(tool, timer_service_start_stopwatch(), id);
    }
    if (strcmp(tool, MCP_TOOL_PAUSE_STOPWATCH) == 0)
    {
        return handle_stopwatch(tool, timer_service_pause_stopwatch(), id);
    }
    if (strcmp(tool, MCP_TOOL_STOP_STOPWATCH) == 0)
    {
        return handle_stopwatch(tool, timer_service_stop_stopwatch(), id);
    }
    if (strcmp(tool, MCP_TOOL_CHARGE_PLAN) == 0)
    {
        return handle_charge_get_plan(id);
    }
    if (strcmp(tool, MCP_TOOL_NEWS_FETCH) == 0)
    {
        return handle_news_fetch(arguments, id);
    }
    if (strcmp(tool, MCP_TOOL_STOP_RINGING) == 0)
    {
        return handle_stop_ringing(id);
    }
    if (strcmp(tool, MCP_TOOL_RADIO_PLAY) == 0)
    {
        return handle_radio_start(false, id);
    }
    if (strcmp(tool, MCP_TOOL_RADIO_NEXT) == 0)
    {
        return handle_radio_start(true, id);
    }
    if (strcmp(tool, MCP_TOOL_RADIO_PAUSE) == 0)
    {
        return handle_radio_command(tool, radio_stream_service_pause(), id);
    }
    if (strcmp(tool, MCP_TOOL_RADIO_RESUME) == 0)
    {
        return handle_radio_command(tool, radio_stream_service_resume(), id);
    }
    if (strcmp(tool, MCP_TOOL_RADIO_STOP) == 0)
    {
        return handle_radio_command(tool, radio_stream_service_stop(), id);
    }
    ESP_LOGW(TAG, "tool call rejected: unknown tool %s", tool);
    return reply_error(id, MCP_ERROR_METHOD_NOT_FOUND, "Unknown tool");
}

static const char *handle_tools_list(long id)
{
    /* The catalogue is a static literal (see k_tools_list_result_json), so the
     * answer is one bounded copy instead of a cJSON build per request. */
    return reply_result_raw(id, k_tools_list_result_json);
}

static const char *handle_initialize(long id)
{
    return reply_result_raw(id,
                            "{\"protocolVersion\":\"" MCP_PROTOCOL_VERSION "\","
                            "\"capabilities\":{\"tools\":{}},"
                            "\"serverInfo\":{\"name\":\"" MCP_SERVER_NAME
                            "\",\"version\":\"" MCP_SERVER_VERSION "\"}}");
}

const char *mcp_service_handle(const cJSON *payload)
{
    if (!cJSON_IsObject(payload))
    {
        ESP_LOGW(TAG, "MCP frame rejected: payload is not an object");
        return NULL;
    }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(payload, "jsonrpc");
    if (!cJSON_IsString(version) || version->valuestring == NULL ||
        strcmp(version->valuestring, "2.0") != 0)
    {
        ESP_LOGW(TAG, "MCP frame rejected: unsupported jsonrpc version");
        return NULL;
    }
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(payload, "method");
    if (!cJSON_IsString(method) || method->valuestring == NULL)
    {
        ESP_LOGW(TAG, "MCP frame rejected: missing method");
        return NULL;
    }
    /* Notifications carry no id and must never be answered; the same guard drops
     * malformed requests the cloud could not match a reply to anyway. */
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(payload, "id");
    if (!cJSON_IsNumber(id))
    {
        ESP_LOGI(TAG, "MCP notification ignored: method=%s", method->valuestring);
        return NULL;
    }
    const long request_id = (long)id->valueint;
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(payload, "params");
    ESP_LOGI(TAG, "MCP request: method=%s id=%ld", method->valuestring, request_id);

    if (strcmp(method->valuestring, "initialize") == 0)
    {
        const char *reply = handle_initialize(request_id);
        if (reply != NULL)
        {
            s_discovery_done = true;
            ESP_LOGI(TAG, "MCP discovery completed; the cloud now owns the volume, timer and radio control");
        }
        return reply;
    }
    if (strcmp(method->valuestring, "tools/list") == 0)
    {
        const char *reply = handle_tools_list(request_id);
        if (reply != NULL)
        {
            s_discovery_done = true;
            ESP_LOGI(TAG, "MCP discovery completed; the cloud now owns the volume, timer and radio control");
        }
        return reply;
    }
    if (strcmp(method->valuestring, "tools/call") == 0)
    {
        return handle_tools_call(params, request_id);
    }
    ESP_LOGW(TAG, "MCP method not implemented: %s", method->valuestring);
    return reply_error(request_id, MCP_ERROR_METHOD_NOT_FOUND, "Method not implemented");
}

bool mcp_service_is_ready(void)
{
    return s_discovery_done;
}

void mcp_service_reset(void)
{
    s_discovery_done = false;
    /* A deferred start that was still waiting when the session ended must not
     * leak into the next one. */
    s_pending_countdown = false;
    s_pending_radio = false;
    s_pending_radio_next = false;
    s_radio_tools_blocked = false;
}

bool mcp_service_pending_countdown(void)
{
    return s_pending_countdown;
}

void mcp_service_clear_pending_countdown(void)
{
    s_pending_countdown = false;
}

bool mcp_service_pending_radio(void)
{
    return s_pending_radio;
}

void mcp_service_clear_pending_radio(void)
{
    s_pending_radio = false;
    s_pending_radio_next = false;
}

/* 这次待启动的是"换一个台"而不是"打开电台"。调用方据此在 play_next() 和
   play_default() 之间选，频道表本身仍然只有 radio_stream_service 知道。 */
bool mcp_service_pending_radio_next(void)
{
    return s_pending_radio_next;
}

void mcp_service_set_radio_tools_blocked(bool blocked)
{
    s_radio_tools_blocked = blocked;
}
