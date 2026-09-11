$ErrorActionPreference = 'Stop'
$driver = Get-Content -Raw "$PSScriptRoot\..\main\Dri\i2s_audio_driver.c"
$pipeline = Get-Content -Raw "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$voice = Get-Content -Raw "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$iot = Get-Content -Raw "$PSScriptRoot\..\main\MID\system\voice_iot_command_service.c"
$iotBytes = [BitConverter]::ToString([IO.File]::ReadAllBytes((Join-Path $PSScriptRoot '..\main\MID\system\voice_iot_command_service.c')))
if ($driver -notmatch 'ES8311_MIC_INPUT_GAIN_DB 24\.0f') { throw 'microphone gain must use the 24 dB far-field pickup value' }
foreach ($pattern in @('capture_rms','capture_peak','capture_frames','capture_bytes','capture_cache_bytes','capture_duration_us','capture_short','capture_low_level')) {
    if ($pipeline -notmatch $pattern -and $voice -notmatch $pattern) { throw "missing per-turn capture diagnostic: $pattern" }
}
if (!$iotBytes.Contains('E4-B8-8D') -or $iot -notmatch 'negation') { throw 'negative volume STT must be rejected before substring matching' }
if ($voice -notmatch 'XIAOZHI_UPLINK_MAIN_WRITE_WAIT_TIMEOUT_US' -or $voice -notmatch 'in-flight bin-main completed') { throw 'terminal path must wait for in-flight bin-main' }

# 起手预卷。实测能量门限把用户开口的头一段整段丢掉（日志 20.24 s 开口、21.28 s
# 才确认，“关闭电台”只剩下“电台。”上云，云端只好自己去猜动词）。门限和防抖都
# 不能动（环境噪声经常越过 500，800 才稳），所以要在 AFE 输出侧把那段音频留住，
# 确认时一次性补发。
foreach ($pattern in @(
    '#define XIAOZHI_PREROLL_BYTES 49152U',
    'static void preroll_push\(const uint8_t \*data, size_t bytes\)',
    'static void preroll_drain\(void\)',
    'static void preroll_reset\(void\)',
    'preroll_push\(\(const uint8_t \*\)result->data, \(size_t\)result->data_size\);',
    's_pipeline\.preroll_buffer = heap_caps_malloc\(XIAOZHI_PREROLL_BYTES, MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT\);',
    'heap_caps_free\(s_pipeline\.preroll_buffer\);',
    'preroll replay: bytes=%u ms=%u'
)) {
    if ($pipeline -notmatch $pattern) { throw "missing pre-roll capture piece: $pattern" }
}
# 收集点必须在能量门限/防抖那几条 continue 之前，否则被挡掉的正好是要救的那一段。
$collectAt = $pipeline.IndexOf('preroll_push((const uint8_t *)result->data')
$gateAt = $pipeline.IndexOf('VAD start gated by energy:')
if ($collectAt -lt 0 -or $gateAt -lt 0 -or $collectAt -gt $gateAt) {
    throw 'The pre-roll capture must sit ahead of the energy and debounce gates, or the frames it exists to save are the ones it never sees.'
}
# 补发必须在 ++stream_epoch 之后，否则编码任务会把它们当陈旧帧丢掉。
$epochAt = $pipeline.IndexOf('++s_pipeline.stream_epoch;')
$drainAt = $pipeline.IndexOf('preroll_drain();')
if ($drainAt -lt 0 -or $drainAt -lt $epochAt) {
    throw 'The pre-roll replay must run after ++stream_epoch or every replayed frame is dropped as stale.'
}
# 队列要装得下整段预卷（48 KB / 1920 B ≈ 26 帧），否则补发时先把最旧的丢掉。
if ($pipeline -notmatch '#define PCM_FRAME_QUEUE_DEPTH 32U') {
    throw 'The PCM handoff queue must hold one full pre-roll, or the replay drops its own beginning.'
}
# RESET_AFTER_SILENCE 里不能清预卷。follow-up 直接接话是“重置基线 -> 立刻开流”：
# 语音服务在 recent_peak 过了 FOLLOWUP_DIRECT_REPLY_MIN_PEAK 之后先发
# RESET_AFTER_SILENCE 再发 START_STREAM。实测日志里每一个
# "voice AFE reset after silence" 后面都紧跟 "voice stream start"，
# preroll replay 一次都没打出来过，刚攒的那一秒全被这里倒掉了。
$resetCaseAt = $pipeline.IndexOf('case VOICE_AUDIO_CONTROL_RESET_AFTER_SILENCE:')
$nextCaseAt = $pipeline.IndexOf('case VOICE_AUDIO_CONTROL_RESUME_AFTER_PLAYBACK:')
if ($resetCaseAt -lt 0 -or $nextCaseAt -le $resetCaseAt) {
    throw 'RESET_AFTER_SILENCE must keep its own case block ahead of the playout one.'
}
if ($pipeline.Substring($resetCaseAt, $nextCaseAt - $resetCaseAt) -match 'preroll_reset\(\)') {
    throw 'RESET_AFTER_SILENCE must not drop the pre-roll: it runs right before the follow-up stream start.'
}
# 设备自己出声期间不能采集预卷，否则“下次再聊”这类提示音会被当成用户刚说的话
# 补发上去。playout_active 在播放事件里置位，在窗口边界由 preroll_reset 兜底复位
# （abort 不发事件，不兜底会永久卡住）。
if ($pipeline -notmatch '!s_pipeline\.streaming && !s_pipeline\.playout_active && result->data_size > 0') {
    throw 'The pre-roll capture must stop while the device itself is playing.'
}
if ($pipeline -notmatch 'if \(event == AUDIO_PLAYOUT_EVENT_STARTED\) s_pipeline\.playout_active = true;') {
    throw 'The playout event must mark the device-speaking window.'
}
if ($pipeline -notmatch 's_pipeline\.playout_active = false;') {
    throw 'The device-speaking window needs a reset path, or a stray abort leaves the pre-roll permanently dead.'
}
Write-Output 'Voice capture accuracy contract passed.'
