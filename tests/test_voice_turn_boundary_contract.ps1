$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

@(
    @{ Pattern = 'static bool s_wait_for_turn_silence;'; Message = 'The voice service needs one shared silence-boundary gate for wake and follow-up turns.' },
    @{ Pattern = 's_wait_for_turn_silence = true;'; Message = 'A WakeNet-triggered cloud turn must wait for the wake-word tail to become silent.' },
    @{ Pattern = 'voice turn silence boundary observed; ready for question'; Message = 'The common turn boundary must be observable in logs.' },
    @{ Pattern = 'wake-word VAD start ignored; waiting for silence boundary'; Message = 'The wake-word speech itself must not be accepted as the cloud question.' }
) | ForEach-Object {
    if ($voice -notmatch $_.Pattern) { throw $_.Message }
}

$vadStart = $voice.IndexOf('case VOICE_EVENT_VAD_START:')
$vadEnd = $voice.IndexOf('case VOICE_EVENT_VAD_END:', $vadStart)
if ($vadStart -lt 0 -or $vadEnd -lt 0) {
    throw 'VAD event state-machine boundaries are missing.'
}
$vadStartBlock = $voice.Substring($vadStart, $vadEnd - $vadStart)
if ($vadStartBlock -notmatch 's_state == XIAOZHI_VOICE_WAIT_SPEECH && s_wait_for_turn_silence') {
    throw 'Initial wake-word VAD must be gated by the same silence boundary as follow-up speech.'
}

$sendDetect = $voice.IndexOf('static void send_detect(bool start_current_speech)')
$startWss = $voice.IndexOf('static esp_err_t start_wss', $sendDetect)
$sendDetectBlock = $voice.Substring($sendDetect, $startWss - $sendDetect)
if ($sendDetectBlock -notmatch 'static void send_detect\(bool start_current_speech\)' -or
    $sendDetectBlock -notmatch 'if \(start_current_speech\) start_listen\(true\);') {
    throw 'The detect helper must explicitly distinguish a wake-word tail from an already-validated follow-up question.'
}
if ($sendDetectBlock -match 'if \(s_vad_speech\) start_listen\(\);') {
    throw 'send_detect must not infer that every VAD-active input is a question.'
}

$followupStart = $voice.IndexOf('if (s_state == XIAOZHI_VOICE_WAIT_FOLLOWUP)')
$followupEnd = $voice.IndexOf('} else if (s_state == XIAOZHI_VOICE_WAIT_SPEECH', $followupStart)
$followupBlock = $voice.Substring($followupStart, $followupEnd - $followupStart)
if ($followupBlock -match 'send_detect\(' -or
    $followupBlock -notmatch 'start_followup_listen\(\);') {
    throw 'A VAD start after the silence boundary must start direct follow-up streaming without resending wake-word detect text.'
}
if ($voice -notmatch 'send_detect\(false\);') {
    throw 'WakeNet and WSS hello paths must keep the wake-word tail out of the audio uplink.'
}

Write-Output 'Voice turn-boundary contract passed.'
