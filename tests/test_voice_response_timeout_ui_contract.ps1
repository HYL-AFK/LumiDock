$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$ui = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\ui_service.c"
$snapshot = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\mid_snapshot.h"
$iot = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\voice_iot_command_service.c"

function Get-Block($content, $startNeedle, $endNeedle) {
    $start = $content.IndexOf($startNeedle)
    if ($start -lt 0) { throw "Missing block start: $startNeedle" }
    $end = $content.IndexOf($endNeedle, $start)
    if ($end -lt 0) { throw "Missing block end: $endNeedle" }
    return $content.Substring($start, $end - $start)
}

# The cloud response deadline must surface a dedicated UI state instead of
# silently dropping back to the main page.
if ($snapshot -notmatch 'VOICE_UI_TIMEOUT') {
    throw 'mid_snapshot must define VOICE_UI_TIMEOUT so the voice UI can show a cloud response timeout hint.'
}
if ($ui -notmatch 'state == VOICE_UI_TIMEOUT') {
    throw 'The timeout state must have a dedicated voice eye branch.'
}
if ($ui -notmatch 'state == VOICE_UI_TIMEOUT[\s\S]*?UI_VOICE_THINK_EYE_WIDTH') {
    throw 'The timeout state must share the thinking eye width so the hint is visible in the face UI.'
}

# A real cloud turn that times out must request a spoken timeout prompt from
# the cloud; when the WSS is unusable it must answer with the local error tone
# and close the session only after the tone finishes, instead of bouncing back
# to the main page silently.
$sessionTask = Get-Block $voice 'static void voice_session_task' 'static bool wait_for_prewarmed_wss'
if ($sessionTask -notmatch 'cloud data acquisition timeout; requesting generated timeout prompt') {
    throw 'Cloud data acquisition timeout must request the generated timeout prompt.'
}
if ($sessionTask -notmatch 'request_cloud_timeout_prompt()') {
    throw 'Cloud data acquisition timeout must request a cloud-spoken timeout prompt.'
}
if ($sessionTask -notmatch 'play_error_tone_then_close\(true\);') {
    throw 'Cloud data acquisition timeout must answer with the local error tone when the prompt cannot be requested.'
}
if ($voice -notmatch 'prompt unavailable; playing local error tone then closing the session') {
    throw 'Prompt failure must log that the local error tone answers before the session closes.'
}
if ($voice -notmatch 's_state\s*=\s*XIAOZHI_VOICE_ERROR_TONE;') {
    throw 'Prompt failure must enter the error tone state instead of wake-wait.'
}
if ($voice -notmatch 'local error tone finished; closing the voice session') {
    throw 'The error tone FINISHED event must close the session and return to the main page.'
}
if ($voice -notmatch 'local error tone watchdog expired; closing the voice session') {
    throw 'The error tone must have a watchdog so a stuck playout can never hang the session.'
}
if ($voice -match 'XIAOZHI_TIMEOUT_UI_HOLD_US|s_timeout_ui_deadline_us') {
    throw 'The five second timeout hold that bounced back to the main page must be removed.'
}

# The proactive exit prompt is driven by the follow-up silence window alone,
# never by user keyword matching in the local IoT command layer.
if ($voice -notmatch 'request_cloud_exit_prompt') { throw 'Exit prompt support must remain available for completed idle sessions.' }
if ($iot -match 'farewell|再见|拜拜|xiaozhi_voice_service_request_exit_prompt') {
    throw 'The IoT command layer must not trigger exit prompts from user keywords.'
}

Write-Output 'Voice response timeout UI contract passed.'
