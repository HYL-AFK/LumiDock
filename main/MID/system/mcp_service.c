#include "mcp_service.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "device_settings_service.h"
#include "i2s_audio_driver.h"
#include "radio_stream_service.h"
#include "timer_service.h"

static const char *const TAG = "mcp";

/* The tools/list reply is the widest frame we ever build (every tool carries
 * its full description), so the buffer is sized for it with headroom.  It lives
 * in .bss so the websocket session task stack stays untouched, and the voice
 * service envelope buffer (XIAOZHI_MCP_FRAME_MAX_BYTES) wraps it. */
#define MCP_REPLY_MAX_BYTES 4096U

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
#define MCP_TOOL_RADIO_PAUSE "self.radio.pause"
#define MCP_TOOL_RADIO_RESUME "self.radio.resume"
#define MCP_TOOL_RADIO_STOP "self.radio.stop"

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
    "Set the speaker volume to an absolute level 0-100. Call self.get_device_status "
    "first: the value is applied verbatim, so a guess is heard at once. Use this "
    "only when the user names a level. Relative requests such as turning it up or "
    "asking for less volume are handled by the device itself and must not be "
    "forwarded here."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"volume\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},"
    "\"required\":[\"volume\"]}},"
    "{\"name\":\"" MCP_TOOL_SET_ALARM "\",\"description\":\""
    "Set a one-shot alarm that rings at the next occurrence of hour:minute local time "
    "(tomorrow when that moment already passed). The device clock must be synchronized. "
    "hour is 0-23 and minute is 0-59. There is a single alarm slot, so a new alarm "
    "replaces the pending one. The result names the alarm that was replaced, and you "
    "must tell the user which alarm was cancelled."
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
    "Start the built-in live radio. Omit station for the default channel. "
    "Channels: 中国之声 news (default), 经济之声 business, 音乐之声 music, "
    "文艺之声 culture. To switch without naming one, take the current channel "
    "from self.get_device_status and pass the next name in that list, wrapping "
    "around. Never ask which genre the user wants."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"station\":"
    "{\"type\":\"string\",\"description\":\"Channel name; omit for the default.\"}}}}",
    "{\"name\":\"" MCP_TOOL_RADIO_PAUSE "\",\"description\":\""
    "Pause the radio stream. Use self.radio.resume to continue it."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"" MCP_TOOL_RADIO_RESUME "\",\"description\":\""
    "Resume the paused radio stream."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"" MCP_TOOL_RADIO_STOP "\",\"description\":\""
    "Stop the radio stream and release the audio channel."
    "\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"
    "]}";

/* A truncated tools/list answer silently disables every device control, so the
 * catalogue is verified against the reply buffer at compile time. */
_Static_assert(sizeof(k_tools_list_result_json) + 64U <= MCP_REPLY_MAX_BYTES,
               "MCP tool catalogue must fit the reply buffer");

static char s_reply[MCP_REPLY_MAX_BYTES];

/* Set once the cloud has seen the tool list; the local command shortcuts read
 * it through mcp_service_is_ready() to avoid applying a change twice. */
static bool s_discovery_done;

/* Deferred starts requested by a tool call (see mcp_service.h).  Both are only
 * touched from the voice session task, so no lock is needed. */
static volatile bool s_pending_countdown;
static volatile bool s_pending_radio;
/* 云端在 self.radio.play 里点名要哪个频道。空串＝没点名（放默认台）。
 * 只存名字不存下标：真正解析在 radio_stream_service 里做，那里才拿得到频道表；
 * 而且名字比下标更能承受编译期的频道增删。 */
static char s_pending_radio_station[24];
/* Set once a local radio command has been applied (see mcp_service.h): the
 * cloud keeps talking about the same utterance, and its radio tools must not
 * be allowed to undo what already happened. */
static volatile bool s_radio_tools_blocked;

/* ------------------------------------------------------------------ */
/* JSON-RPC response builders                                          */
/* ------------------------------------------------------------------ */

static const char *reply_result_raw(long id, const char *result_json)
{
    const int written = snprintf(s_reply, sizeof(s_reply),
                                 "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":%s}", id,
                                 result_json);
    if (written <= 0 || (size_t)written >= sizeof(s_reply))
    {
        ESP_LOGE(TAG, "MCP result reply dropped: %d bytes needed", written);
        return NULL;
    }
    return s_reply;
}

static const char *reply_error(long id, int code, const char *message)
{
    const int written = snprintf(s_reply, sizeof(s_reply),
                                 "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
                                 id, code, message);
    if (written <= 0 || (size_t)written >= sizeof(s_reply))
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
    const int written = snprintf(
        s_reply, sizeof(s_reply),
        "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],\"isError\":false}}",
        id, json_escaped_text);
    if (written <= 0 || (size_t)written >= sizeof(s_reply))
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

static const char *handle_radio_play(const cJSON *arguments, long id)
{
    const cJSON *station =
        cJSON_IsObject(arguments) ? cJSON_GetObjectItemCaseSensitive(arguments, "station") : NULL;
    /* 台名是可选参数：只部署“打开电台”的说法完全合法，那就放默认台。
       给了但不是字符串才当参数错。 */
    if (station != NULL && !cJSON_IsString(station))
    {
        ESP_LOGW(TAG, "tool call rejected: %s with a non-string station", MCP_TOOL_RADIO_PLAY);
        return reply_error(id, MCP_ERROR_INVALID_PARAMS, "Invalid argument: station");
    }
    const char *wanted = (station != NULL && station->valuestring != NULL) ? station->valuestring : "";
    strncpy(s_pending_radio_station, wanted, sizeof(s_pending_radio_station) - 1U);
    s_pending_radio_station[sizeof(s_pending_radio_station) - 1U] = '\0';
    /* The cloud answers this call with speech, and that TTS owns the shared
     * playout; starting the stream here would be silenced by it. */
    s_pending_radio = true;
    ESP_LOGI(TAG, "tool call: %s station=\"%s\" deferred until the acknowledgement TTS drains",
             MCP_TOOL_RADIO_PLAY, s_pending_radio_station);
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

static bool is_radio_tool(const char *tool)
{
    return strcmp(tool, MCP_TOOL_RADIO_PLAY) == 0 ||
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
    if (strcmp(tool, MCP_TOOL_STOP_RINGING) == 0)
    {
        return handle_stop_ringing(id);
    }
    if (strcmp(tool, MCP_TOOL_RADIO_PLAY) == 0)
    {
        return handle_radio_play(arguments, id);
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
    s_pending_radio_station[0] = '\0';
}

/* 空串＝云端没点名，调用方放默认台。 */
const char *mcp_service_pending_radio_station(void)
{
    return s_pending_radio_station;
}

void mcp_service_set_radio_tools_blocked(bool blocked)
{
    s_radio_tools_blocked = blocked;
}
