$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$header = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.h"

if ($voice -match '\[device_call\]您请说|\[device_call\].*正听着') {
    throw 'Normal wake must not use a device_call prompt.'
}
if ($voice -notmatch 'XIAOZHI_WAKE_PRIMER_FRAMES 35U') {
    throw 'Normal wake must prime the server just_woken_up window with about 2.1 seconds of Opus silence.'
}
if ($voice -notmatch 'wake server ignore primer started' -or
    $voice -notmatch 'wake server ignore primer done') {
    throw 'Wake primer start and completion must be visible in logs.'
}
if ($voice -notmatch 's_listen_start_sent_for_turn') {
    throw 'The voice service must track whether listen/start was already sent for a turn.'
}
if ($voice -notmatch 'start_listen\(false\)' -or $voice -notmatch 'start_listen\(true\)') {
    throw 'Wake primer must start listen/start separately from real microphone capture.'
}
if ($pipeline -notmatch 'xiaozhi_audio_pipeline_queue_silence_opus_frames' -or
    $header -notmatch 'xiaozhi_audio_pipeline_queue_silence_opus_frames') {
    throw 'The audio pipeline must expose an Opus silence-frame queue for wake primer.'
}

$startListenStart = $voice.IndexOf('static void start_listen')
$startListenEnd = $voice.IndexOf('static void finish_boot_greeting_failure', $startListenStart)
if ($startListenStart -lt 0 -or $startListenEnd -lt 0) { throw 'start_listen block is missing.' }
$startListenBlock = $voice.Substring($startListenStart, $startListenEnd - $startListenStart)
if ($startListenBlock -notmatch 'if \(!s_listen_start_sent_for_turn\)' -or
    $startListenBlock -notmatch 'enable_capture') {
    throw 'start_listen must avoid sending duplicate listen/start when wake primer already opened the server turn.'
}

Write-Output 'Voice wake primer contract passed.'
