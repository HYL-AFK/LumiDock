$ErrorActionPreference = 'Stop'

$iot = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\voice_iot_command_service.c"
$hdr = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\voice_iot_command_service.h"

# The execute signature must carry an announce output buffer.
if ($hdr -notmatch 'voice_iot_command_service_execute\(' -or
    $hdr -notmatch 'char \*announce, size_t announce_size') {
    throw 'The IoT execute signature must accept an announce buffer + size.'
}

# Chinese digits 0-9 plus 两, and Arabic digits via strtoul.
if ($iot -notmatch 'chinese_digit_value' -or
    $iot -notmatch '\{"零", 0U\}' -or
    $iot -notmatch '\{"两", 2U\}' -or
    $iot -notmatch '\{"九", 9U\}' -or
    $iot -notmatch 'strtoul\(digits, NULL, 10\)') {
    throw 'Number parsing must cover Chinese digits (零-九, 两) and Arabic digits.'
}

# Chinese tens: 十=10, 十五=15, 二十=20, 三十五=35.
$parseNumStart = $iot.IndexOf('static bool parse_number')
$parseClockStart = $iot.IndexOf('static bool parse_clock_hh_mm')
if ($parseNumStart -lt 0 -or $parseClockStart -lt 0) { throw 'parse_number boundaries are missing.' }
$numBlock = $iot.Substring($parseNumStart, $parseClockStart - $parseNumStart)
if ($numBlock -notmatch '十 = 10, 十五 = 15' -or
    $numBlock -notmatch 'value = 10U \+ ones' -or
    $numBlock -notmatch 'value = tens \* 10U' -or
    $numBlock -notmatch 'value \+= ones') {
    throw 'Chinese tens parsing (10-99) must handle 十/十五/二十/三十五 forms.'
}

# Clock parsing: 点 separator, 半 = 30, half/full width colon, 分 suffix.
$clockStart = $iot.IndexOf('static bool parse_clock_hh_mm')
$findStart = $iot.IndexOf('static bool find_clock_in_text')
$clockBlock = $iot.Substring($clockStart, $findStart - $clockStart)
if ($clockBlock -notmatch 'strncmp\(p, "点", 3\) == 0' -or
    $clockBlock -notmatch '\*p == '':''' -or
    $clockBlock -notmatch 'strncmp\(p, "：", 3\) == 0' -or
    $clockBlock -notmatch 'strncmp\(p, "半", 3\) == 0' -or
    $clockBlock -notmatch 'minute = 30U' -or
    $clockBlock -notmatch 'minute <= 59U') {
    throw 'Clock parsing must support 点/半/full-width colon and clamp minutes to 59.'
}
# 下午/晚上 shifts 1-11 to 13-23.
$findBlock = $iot.Substring($findStart, $iot.IndexOf('MAX_DURATION_SECONDS') - $findStart)
if ($findBlock -notmatch 'strstr\(text, "下午"\)' -or
    $findBlock -notmatch 'strstr\(text, "晚上"\)' -or
    $findBlock -notmatch '\*hour_out \+ 12U') {
    throw '下午/晚上 must shift hours 1-11 to 13-23.'
}

# Duration parsing: hours/minutes/seconds plus 半 forms and the 7-day cap.
$durStart = $iot.IndexOf('static bool parse_duration_seconds')
$setAnnounceStart = $iot.IndexOf('static void set_announce')
$durBlock = $iot.Substring($durStart, $setAnnounceStart - $durStart)
if ($durBlock -notmatch 'strncmp\(p, "小时", 6\) == 0' -or
    $durBlock -notmatch 'strncmp\(p, "分钟", 6\) == 0' -or
    $durBlock -notmatch 'strncmp\(p, "秒钟", 6\) == 0' -or
    $durBlock -notmatch 'strncmp\(p, "秒", 3\) == 0' -or
    $durBlock -notmatch 'strncmp\(p, "半小时", 9\) == 0' -or
    $durBlock -notmatch 'strncmp\(p, "半分钟", 9\) == 0' -or
    $durBlock -notmatch '一个半小时 = 1h \+ 30m') {
    throw 'Duration parsing must cover 小时/分钟/秒钟/秒 and 半小时/半分钟/一个半小时.'
}
if ($iot -notmatch 'MAX_DURATION_SECONDS \(7U \* 24U \* 3600U\)') {
    throw 'Duration parsing must enforce the 7-day safety cap.'
}

# Announce helper writes through vsnprintf with truncation safety.
if ($iot -notmatch 'vsnprintf\(announce, announce_size, format, args\)' -or
    $iot -notmatch 'announce\[announce_size - 1U\] = ''\\0''') {
    throw 'The announce helper must use vsnprintf and force NUL termination.'
}

# Command vocabulary for ring stop, queries, stopwatch and timers.
foreach ($phrase in @('关掉闹钟', '停止响铃', '别响了', '还剩多久', '计时多久', '停止计时',
                      '暂停计时', '开始计时', '倒计时', '提醒我', '叫醒我')) {
    if ($iot -notmatch [regex]::Escape($phrase)) {
        throw "Timer command vocabulary must contain: $phrase"
    }
}

# Command order contract: queries run before stopwatch controls before
# countdown/alarm setters, and ring-stop runs first of all.
$stopRingIdx = $iot.IndexOf('"关掉闹钟", "关闭闹钟", "停止响铃"')
$queryCountIdx = $iot.IndexOf('"还剩多久", "还剩多少"')
$queryStopIdx = $iot.IndexOf('"计时多久", "正计时多久"')
$stopSwIdx = $iot.IndexOf('"停止计时", "结束计时"')
$startSwIdx = $iot.IndexOf('"开始计时", "开始正计时"')
$countdownIdx = $iot.IndexOf('const char *const countdown_phrases')
$alarmIdx = $iot.IndexOf('const char *const alarm_phrases')
if ($stopRingIdx -lt 0 -or $queryCountIdx -lt 0 -or $queryStopIdx -lt 0 -or
    $stopSwIdx -lt 0 -or $startSwIdx -lt 0 -or $countdownIdx -lt 0 -or $alarmIdx -lt 0) {
    throw 'Timer phrase tables must all exist.'
}
if (-not ($stopRingIdx -lt $queryCountIdx -and $queryCountIdx -lt $queryStopIdx -and
          $queryStopIdx -lt $stopSwIdx -and $stopSwIdx -lt $startSwIdx -and
          $startSwIdx -lt $countdownIdx -and $countdownIdx -lt $alarmIdx)) {
    throw 'Command order must be: ring-stop, countdown query, stopwatch query, stopwatch stop/pause/start, countdown set, alarm set.'
}

# Ring stop requires an active ring; otherwise the cloud keeps the phrase.
$execStart = $iot.IndexOf('voice_iot_command_result_t voice_iot_command_service_execute')
$execBlock = $iot.Substring($execStart)
if ($execBlock -notmatch 'if \(!timer_service_is_ringing\(\)\) return VOICE_IOT_COMMAND_NOT_HANDLED;' -or
    $execBlock -notmatch 'timer_service_stop_ringing\(\);') {
    throw 'Ring-stop must no-op (NOT_HANDLED) when nothing rings and stop the ring otherwise.'
}

# Setters call the timer service and fail loudly when parsing is impossible.
if ($execBlock -notmatch 'parse_duration_seconds\(transcript, &seconds\)' -or
    $execBlock -notmatch 'timer_service_prepare_countdown\(seconds\)' -or
    $execBlock -notmatch 'find_clock_in_text\(transcript, &hour, &minute\)' -or
    $execBlock -notmatch 'timer_service_set_alarm\(hour, minute\)') {
    throw 'Countdown/alarm commands must parse the transcript and call the timer service.'
}

# Queries report remaining countdown / elapsed stopwatch via announce.
if ($execBlock -notmatch 'timer_service_query_countdown_remaining\(&remaining\)' -or
    $execBlock -notmatch '"倒计时还剩%u分%u秒"' -or
    $execBlock -notmatch 'timer_service_query_stopwatch\(&elapsed\)' -or
    $execBlock -notmatch '"已经计时%u分%u秒"') {
    throw 'Timer queries must answer through the announce buffer.'
}

# Stopwatch start/pause/stop map onto the timer service.
if ($execBlock -notmatch 'timer_service_start_stopwatch\(\)' -or
    $execBlock -notmatch 'timer_service_pause_stopwatch\(\)' -or
    $execBlock -notmatch 'timer_service_stop_stopwatch\(\)') {
    throw 'Stopwatch commands must map onto the timer service API.'
}

Write-Output 'Voice timer command contract passed.'
