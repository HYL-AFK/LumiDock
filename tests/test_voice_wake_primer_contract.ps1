$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$header = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.h"

if ($voice -match '\[device_call\]您请说|\[device_call\].*正听着') {
    throw 'Normal wake must not use a device_call prompt.'
}
$ttsStart = $voice.IndexOf('if (greeting_turn) {')
$ttsEnd = $voice.IndexOf('if (boot_greeting_turn) {', $ttsStart)
if ($ttsStart -lt 0 -or $ttsEnd -lt 0) { throw 'Manual wake greeting TTS boundaries are missing.' }
$ttsBlock = $voice.Substring($ttsStart, $ttsEnd - $ttsStart)
if ($ttsBlock -match 'start_wake_server_ignore_primer\(\)') {
    throw 'The manual wake greeting must not start the legacy silence primer before PCM drain.'
}
if ($voice -notmatch 's_listen_start_sent_for_turn') {
    throw 'The voice service must track whether listen/start was already sent for a turn.'
}
if ($voice -notmatch 'start_listen\(true\)') {
    throw 'Manual wake must start listen/start only when fresh microphone speech is detected.'
}
if ($pipeline -notmatch 'xiaozhi_audio_pipeline_queue_silence_opus_frames' -or
    $header -notmatch 'xiaozhi_audio_pipeline_queue_silence_opus_frames') {
    throw 'The legacy primer helper must remain available without being used during greeting playback.'
}

$startListenStart = $voice.IndexOf('static void start_listen')
$startListenEnd = $voice.IndexOf('static void finish_boot_greeting_failure', $startListenStart)
if ($startListenStart -lt 0 -or $startListenEnd -lt 0) { throw 'start_listen block is missing.' }
$startListenBlock = $voice.Substring($startListenStart, $startListenEnd - $startListenStart)
if ($startListenBlock -notmatch 'if \(!s_listen_start_sent_for_turn\)' -or
    $startListenBlock -notmatch 'enable_capture') {
    throw 'start_listen must avoid duplicate listen/start and enable capture only for a fresh VAD turn.'
}

Write-Output 'Voice wake primer isolation contract passed.'
