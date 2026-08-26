$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

$followupStart = $voice.IndexOf('if (s_state == XIAOZHI_VOICE_WAIT_FOLLOWUP)')
$followupEnd = $voice.IndexOf('} else if (s_state == XIAOZHI_VOICE_WAIT_SPEECH', $followupStart)
if ($followupStart -lt 0 -or $followupEnd -lt 0) {
    throw 'The follow-up VAD-start state-machine boundary is missing.'
}
$followupBlock = $voice.Substring($followupStart, $followupEnd - $followupStart)

if ($followupBlock -match 'send_detect\(') {
    throw 'A follow-up question must not resend the fixed wake-word detect text to the cloud.'
}
if ($followupBlock -notmatch 'start_followup_listen\(\);') {
    throw 'A validated follow-up VAD start must enter direct listen/start streaming.'
}

$helperStart = $voice.IndexOf('static void start_followup_listen(void)')
$helperEnd = $voice.IndexOf('static esp_err_t start_wss', $helperStart)
if ($helperStart -lt 0 -or $helperEnd -lt 0) {
    throw 'The direct follow-up listen helper is missing.'
}
$helper = $voice.Substring($helperStart, $helperEnd - $helperStart)
if ($helper -notmatch 's_state = XIAOZHI_VOICE_WAIT_SPEECH;' -or
    $helper -notmatch 'start_listen\(true\);' -or
    $helper -match 'listen\\\",\\\"state\\\":\\\"detect') {
    throw 'Direct follow-up must use listen/start without a second detect payload.'
}

Write-Output 'Follow-up cloud-protocol contract passed.'
