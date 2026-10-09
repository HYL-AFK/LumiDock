$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

@(
    @{ Pattern = 'static esp_err_t start_wss\(bool foreground_session\)'; Message = 'Voice service must expose a WSS startup helper that separates socket setup from a foreground conversation.' },
    @{ Pattern = 'static void prewarm_wss\(void\)'; Message = 'Voice service must prewarm the WebSocket before a WakeNet event.' },
    @{ Pattern = 'prewarm_wss\(\);'; Message = 'Voice initialization must prewarm the WebSocket while internal RAM is still contiguous.' },
    # 这条锁的是"云端 hello 之后只有真的有待处理唤醒才开麦，且不把唤醒词尾巴推上去"，
    # 不是某一种大括号摆法。原来要求 `if (s_pending_wake) {` 必须同一行，源码换成
    # 换行写法之后断言就永久变红——契约跟着排版漂移等于没有契约。
    @{ Pattern = 'if \(s_pending_wake\)\s*\{[\s\S]*?send_detect\(false\);[\s\S]*?s_pending_wake = false;'; Message = 'Cloud hello must only start detection for a real pending wake word without streaming the wake-word tail.' },
    @{ Pattern = 'start_wss\(true\)'; Message = 'A wake event must promote or create a foreground voice session.' },
    @{ Pattern = 'start_wss\(false\)'; Message = 'Prewarm must not activate the voice UI or listening state.' },
    # 排不上队时不能静默丢弃。断网时 activation_service_network_lost() 会把凭据清掉，
    # 那一瞬间排的这次重连是“唯一一次”——丢掉之后没有任何人会补。实测：电台在播、
    # 断网 27 秒、网络早就回来了，设备手上依然一条 WSS 链路都没有；用户唤醒时只能
    # 冷启动去撞最差的网络，DNS 直接 EAI_FAIL，全程只响一声错误音。
    @{ Pattern = 'if \(!s_initialized\)\s*return;[\s\S]{0,1200}?if \(!xiaozhi_activation_service_is_activated\(\)\)\s*\{\s*ESP_LOGI\(TAG, "WSS idle prewarm deferred: activation unavailable"\);'; Message = 'A prewarm that cannot be queued while the credentials are temporarily gone must be logged, not dropped in silence.' },
    @{ Pattern = 'const bool prewarm_radio_on =\s*radio_starting_after_close \|\| radio_stream_service_is_requested\(\);[\s\S]{0,400}?s_prewarm_retry_us = esp_timer_get_time\(\) \+ prewarm_delay_us;'; Message = 'A disconnected voice socket must retry in the background without refreshing credentials.' },
    @{ Pattern = 'XIAOZHI_PREWARM_RADIO_DELAY_US'; Message = 'Radio playback must keep a hot WebSocket instead of paying a cold TLS handshake on every wake.' },
    # 同上：锁的是"让位后再启的电台也算电台"，不是参数叫什么名字。这段逻辑原本内联在
    # close_channel() 里，参数叫 release_radio_after_close；后来抽成 schedule_idle_prewarm()
    # 时参数改名为 radio_starting_after_close，断言没跟着走就一直红着。
    @{ Pattern = '(?:radio_starting_after_close|release_radio_after_close) \|\| radio_stream_service_is_requested\(\)'; Message = 'A radio start deferred until after the voice chain closes must prewarm immediately too.' },
    @{ Pattern = 'const bool prewarm_radio_on = radio_stream_service_is_requested\(\);'; Message = 'The idle-wake prewarm schedule must also be radio aware.' }
) | ForEach-Object {
    if ($voice -notmatch $_.Pattern) { throw $_.Message }
}

# 上面锁的是“排不上要留痕”，这里锁“有人会补”。空闲预连接必须是常驻不变式：
# 只要还闲着、手上没有连接、凭据也在，主循环就一定把它捡回来。少了这一条，
# 断网期间被推迟的那次重连就永远没人补——那正是上面那条日志描述的现场。
$fireIndex = $voice.IndexOf('s_prewarm_retry_us = 0;')
$invariantIndex = $voice.IndexOf('s_prewarm_retry_us == 0 && s_client == NULL && !s_session_active')
if ($fireIndex -lt 0) {
    throw 'The idle prewarm must be fired from the session loop when its timer expires.'
}
if ($invariantIndex -lt 0) {
    throw 'The idle prewarm must be a standing invariant of the session loop, not a one-shot event.'
}
# 顺序也要对：先“到期就发”，发失败时 prewarm_wss() 会把退避写回 s_prewarm_retry_us。
# 不变式要是排在前面，它会拿退避还没来得及写的那个 0 再排一次，退避就白设了。
if ($invariantIndex -lt $fireIndex) {
    throw 'The standing invariant must run after the timer-expiry prewarm so it cannot trample the failure backoff.'
}
# 不变式必须只排一次：s_prewarm_retry_us == 0 是它的守门条件，掉了这一条就是
# 一个每 20 ms 排一次的忙循环。
if ($voice -notmatch 'if \(s_prewarm_retry_us == 0 && s_client == NULL && !s_session_active &&\s*s_state == XIAOZHI_VOICE_WAIT_WAKE && s_ui_idle_deadline_us == 0 &&\s*xiaozhi_activation_service_is_activated\(\)\)\s*\{\s*schedule_idle_prewarm\(false\);') {
    throw 'The standing invariant must only arm while idle with no client, no active session and the credentials present.'
}

Write-Output 'WebSocket prewarm contract passed.'
