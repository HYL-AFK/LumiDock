$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

function Get-Block($content, $startNeedle, $endNeedle) {
    $start = $content.IndexOf($startNeedle)
    if ($start -lt 0) { throw "Missing block start: $startNeedle" }
    $end = $content.IndexOf($endNeedle, $start)
    if ($end -lt 0) { throw "Missing block end: $endNeedle" }
    return $content.Substring($start, $end - $start)
}

if ($voice -notmatch 'static bool s_boot_greeting_direct_window_active;') {
    throw 'Voice service must track the active boot-greeting direct reply window separately from the first greeting TTS.'
}

$enterFollowup = Get-Block $voice 'static void enter_followup_after_playback(void)' 'static esp_err_t start_wss'
if ($enterFollowup -notmatch 'boot_greeting_window_active[\s\S]*?s_state\s*=\s*XIAOZHI_VOICE_BOOT_GREETING_WAIT_REPLY') {
    throw 'Playback drain inside the boot-greeting 3-minute window must return to BOOT_GREETING_WAIT_REPLY.'
}
if ($enterFollowup -notmatch 's_boot_greeting_direct_window_active\s*=\s*true;') {
    throw 'Initial boot greeting PCM drain must activate the 3-minute direct-reply window.'
}
if ($enterFollowup -notmatch 'boot greeting response PCM drained; direct reply window re-armed=%lldms') {
    throw 'Response TTS drain inside the boot-greeting window must re-arm a fresh 15-second idle window.'
}
if ($enterFollowup -notmatch 's_boot_greeting_reply_deadline_us\s*=\s*esp_timer_get_time\(\)\s*\+\s*XIAOZHI_BOOT_GREETING_REPLY_WINDOW_US') {
    throw 'Response TTS drain inside the boot-greeting window must set a fresh reply deadline.'
}

$processText = Get-Block $voice 'static void process_text' 'static bool receive_uplink_frame'
if ($processText -notmatch 'if \(response_turn\) \{\s*s_boot_greeting_reply_deadline_us\s*=\s*0') {
    throw 'Response TTS start must pause the idle-only boot-greeting reply deadline.'
}
$sessionTask = Get-Block $voice 'static void voice_session_task' 'static bool wait_for_prewarmed_wss'
if ($sessionTask -notmatch 's_boot_greeting_direct_window_active\s*=\s*false;[\s\S]*?boot greeting reply window expired') {
    throw 'Boot-greeting direct window must clear only when the 3-minute deadline expires.'
}

Write-Output 'Boot greeting direct window contract passed.'
