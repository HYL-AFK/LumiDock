$ErrorActionPreference = 'Stop'
$voice = Get-Content (Join-Path $PSScriptRoot '..\main\MID\voice\xiaozhi_voice_service.c') -Raw
$sttStart = $voice.IndexOf('if (strcmp(type->valuestring, "stt") == 0)')
$helloStart = $voice.IndexOf('else if (strcmp(type->valuestring, "hello") == 0)', $sttStart)
if ($sttStart -lt 0 -or $helloStart -lt 0) { throw 'STT processing block is missing.' }
$stt = $voice.Substring($sttStart, $helloStart - $sttStart)
if ($stt -match 's_deferred_announce\s*=\s*true' -or
    $stt -match 'IoT announce deferred until the cloud reply drains') {
    throw 'Local IoT commands must not schedule a second confirmation TTS.'
}
$finishStart = $voice.IndexOf('case VOICE_EVENT_TTS_PLAYBACK_FINISHED:')
$finishEnd = $voice.IndexOf('case VOICE_EVENT_TTS_PLAYBACK_UNDERRUN:', $finishStart)
$finish = $voice.Substring($finishStart, $finishEnd - $finishStart)
if ($finish -match 'request_cloud_announce_prompt') { throw 'Playback drain must not send a second IoT announcement.' }
Write-Output 'Voice IoT single cloud reply contract passed.'
