$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

function Get-Block($content, $startNeedle, $endNeedle) {
    $start = $content.IndexOf($startNeedle)
    if ($start -lt 0) { throw "Missing block start: $startNeedle" }
    $end = $content.IndexOf($endNeedle, $start)
    if ($end -lt 0) { throw "Missing block end: $endNeedle" }
    return $content.Substring($start, $end - $start)
}

$enterFollowup = Get-Block $voice 'static void enter_followup_after_playback(void)' 'static esp_err_t start_wss'
if ($enterFollowup -notmatch 's_wait_for_turn_silence\s*=\s*true;') {
    throw 'Playback drain must arm a post-playback silence gate before accepting follow-up speech.'
}
if ($enterFollowup -match 's_wait_for_turn_silence\s*=\s*false;') {
    throw 'Playback drain must not immediately clear the silence gate and accept speaker tail as speech.'
}
if ($enterFollowup -notmatch 'post-playback silence gate armed') {
    throw 'Playback drain must log that the post-playback silence gate is armed.'
}

$vadStart = Get-Block $voice 'case VOICE_EVENT_VAD_START:' 'case VOICE_EVENT_VAD_END:'
if ($vadStart -notmatch 'XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY[\s\S]*?if \(s_wait_for_turn_silence\)[\s\S]*?break;[\s\S]*?start_listen\(true\)') {
    throw 'Boot greeting direct reply must ignore VAD_START while the post-playback silence gate is armed.'
}
if ($vadStart -notmatch 'XIAOZHI_VOICE_WAIT_FOLLOWUP[\s\S]*?if \(s_wait_for_turn_silence\)[\s\S]*?break;[\s\S]*?start_followup_listen\(\)') {
    throw 'Normal follow-up must ignore VAD_START while the post-playback silence gate is armed.'
}

$vadEnd = Get-Block $voice 'case VOICE_EVENT_VAD_END:' 'case VOICE_EVENT_NETWORK_LOST:'
if ($vadEnd -notmatch 's_wait_for_turn_silence[\s\S]*?s_wait_for_turn_silence\s*=\s*false;[\s\S]*?xiaozhi_audio_pipeline_reset_after_silence\(\);[\s\S]*?post-playback silence observed; ready for question') {
    throw 'VAD_END must clear the post-playback silence gate, reset AFE baseline, and log readiness.'
}

if ($voice -notmatch '#define XIAOZHI_WAIT_RESPONSE_TIMEOUT_US \(10LL \* 1000LL \* 1000LL\)') {
    throw 'WAIT_RESPONSE must use the 10 second bounded cloud response timeout from the known-good baseline.'
}
if ($voice -notmatch 'static int64_t s_response_deadline_us;') {
    throw 'Voice service must track a response deadline while waiting for cloud TTS.'
}

$terminalStop = Get-Block $voice 'static bool send_terminal_uplink_sequence(void)' 'static void finish_listen_with_tail_flush(void)'
if ($terminalStop -notmatch 's_uplink_tx_operation\s*=\s*"listen-stop"' -or
    $terminalStop -notmatch 'send_listen_stop\(\)' -or
    $terminalStop -notmatch 's_response_deadline_us\s*=\s*esp_timer_get_time\(\)\s*\+\s*XIAOZHI_WAIT_RESPONSE_TIMEOUT_US;') {
    throw 'The single terminal owner must send listen/stop and arm the WAIT_RESPONSE timeout on success.'
}

$closeChannel = Get-Block $voice 'static void close_channel(bool refresh_credentials)' 'static bool send_uplink_packet'
if ($closeChannel -notmatch 's_response_deadline_us\s*=\s*0;') {
    throw 'Closing a voice channel must clear the response timeout deadline.'
}

$processText = Get-Block $voice 'static void process_text' 'static bool receive_uplink_frame'
if ($processText -notmatch 's_response_deadline_us\s*=\s*0;') {
    throw 'Receiving cloud TTS must clear the response timeout deadline.'
}
if ($processText -notmatch 'strcmp\(type->valuestring, "stt"\)[\s\S]*?s_response_deadline_us\s*=\s*esp_timer_get_time\(\)\s*\+\s*XIAOZHI_WAIT_RESPONSE_TIMEOUT_US;') {
    throw 'Receiving STT in WAIT_RESPONSE must refresh the response deadline while TTS is generated.'
}

$sessionTask = Get-Block $voice 'static void voice_session_task' 'static bool wait_for_prewarmed_wss'
if ($sessionTask -notmatch 's_state\s*==\s*XIAOZHI_VOICE_WAIT_RESPONSE[\s\S]*?s_response_deadline_us[\s\S]*?cloud response timeout[\s\S]*?close_channel\(false\)') {
    throw 'WAIT_RESPONSE timeout must publish an error through close_channel(false) and recover to wake state.'
}

$beginChannel = Get-Block $voice 'static void begin_channel(void)' 'static void process_text'
if ($beginChannel -notmatch 'start_wss\(true\)\s*!=\s*ESP_OK[\s\S]*?close_channel\(false\)') {
    throw 'Foreground WSS startup failure must enter recoverable voice error UI instead of staying silent.'
}

Write-Output 'Voice post-playback gate and error-state contract passed.'
