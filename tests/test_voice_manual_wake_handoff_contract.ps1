$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

# 锚点必须跟着实现走：TTS 分支早已不再用 `if (greeting_turn) {` / `if (boot_greeting_turn) {`
# 这两个块（改为 guard 变量），旧锚点 IndexOf 返回 -1，直接抛 IndexOutOfRange。
$ttsStart = $voice.IndexOf('strcmp(state->valuestring, "start") == 0')
$ttsEnd = $voice.IndexOf('strcmp(state->valuestring, "stop") == 0', $ttsStart)
if ($ttsStart -lt 0 -or $ttsEnd -lt 0) {
    throw 'Manual wake greeting TTS handoff boundaries are missing.'
}
$ttsBlock = $voice.Substring($ttsStart, $ttsEnd - $ttsStart)
if ($ttsBlock -match 'start_wake_server_ignore_primer\(\)') {
    throw 'Manual wake must not open listen/start or queue a silence primer while the greeting TTS is playing.'
}

$finishStart = $voice.IndexOf('static void finish_wake_greeting_after_playback(void)')
$finishEnd = $voice.IndexOf('static void enter_followup_after_playback(void)', $finishStart)
if ($finishStart -lt 0 -or $finishEnd -lt 0) {
    throw 'Manual wake post-greeting handoff boundaries are missing.'
}
$finishBlock = $voice.Substring($finishStart, $finishEnd - $finishStart)
if ($finishBlock -match 'start_wake_server_ignore_primer\(\)|start_listen\(false\)') {
    throw 'Manual wake must wait for a fresh VAD start instead of using the primer fallback after greeting playback.'
}
if ($finishBlock -notmatch 'XIAOZHI_VOICE_WAIT_FOLLOWUP' -or
    $finishBlock -notmatch 's_followup_deadline_us\s*=\s*esp_timer_get_time\(\) \+ XIAOZHI_FOLLOWUP_WINDOW_US') {
    throw 'Manual wake greeting playback must hand off to the bounded follow-up state.'
}
if ($finishBlock -notmatch 'arm_post_playback_silence_gate\(\)') {
    throw 'Manual wake must arm the same post-playback silence gate as the boot greeting.'
}

$vadEndStart = $voice.IndexOf('case VOICE_EVENT_VAD_END:')
$networkStart = $voice.IndexOf('case VOICE_EVENT_NETWORK_LOST:', $vadEndStart)
if ($vadEndStart -lt 0 -or $networkStart -lt 0) {
    throw 'VAD_END state-machine boundary is missing.'
}
$vadEnd = $voice.Substring($vadEndStart, $networkStart - $vadEndStart)

if ($vadEnd -notmatch 's_wait_for_turn_silence') {
    throw 'Manual wake VAD_END must use the same follow-up silence boundary as boot greeting.'
}

if ($voice -notmatch 'follow-up speech detected; starting next cloud turn' -or
    $voice -notmatch 'start_followup_listen\(\)') {
    throw 'A fresh VAD start in the manual-wake follow-up window must begin cloud capture.'
}

Write-Output 'Manual wake post-greeting handoff contract passed.'
