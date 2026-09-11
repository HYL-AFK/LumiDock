$ErrorActionPreference = 'Stop'

$iot = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\voice_iot_command_service.c"
$timerH = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\timer_service.h"
$timerC = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\timer_service.c"
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

if ($iot -notmatch 'timer_service_prepare_countdown\(seconds\)') {
    throw 'Voice countdown commands must reserve the countdown instead of starting it on STT receipt.'
}
if ($iot -match 'timer_service_set_countdown\(seconds\)') {
    throw 'Voice countdown commands must not call the immediate countdown setter.'
}
if ($timerH -notmatch 'timer_service_prepare_countdown' -or
    $timerH -notmatch 'timer_service_start_pending_countdowns' -or
    $timerH -notmatch 'timer_service_cancel_pending_countdowns') {
    throw 'Timer service must expose prepare/start/cancel APIs for deferred voice countdowns.'
}
if ($timerC -notmatch 'static .*s_pending_countdowns' -or
    $timerC -notmatch 'timer_service_prepare_countdown' -or
    $timerC -notmatch 'timer_service_start_pending_countdowns' -or
    $timerC -notmatch 'timer_service_cancel_pending_countdowns') {
    throw 'Timer service must keep deferred countdowns separate from active countdowns.'
}
if ($voice -notmatch 'VOICE_IOT_COMMAND_COUNTDOWN_PENDING' -or
    $voice -notmatch 's_pending_countdown_start') {
    throw 'Voice service must track a countdown awaiting the current cloud reply.'
}

$finish = $voice.IndexOf('case VOICE_EVENT_TTS_PLAYBACK_FINISHED:')
$finishEnd = $voice.IndexOf('case VOICE_EVENT_TTS_PLAYBACK_UNDERRUN:', $finish)
$closeAt = $voice.IndexOf('static void close_channel(bool')
if ($finish -lt 0 -or $finishEnd -lt 0 -or $closeAt -lt 0) { throw 'Voice playback and close paths are missing.' }
$finishBlock = $voice.Substring($finish, $finishEnd - $finish)
if ($finishBlock -notmatch 'timer_service_start_pending_countdowns\(\)') {
    throw 'A successfully drained cloud TTS must start the pending countdown.'
}
$closeBlock = $voice.Substring($closeAt)
if ($closeBlock -notmatch 'timer_service_start_pending_countdowns\(\)') {
    throw 'Voice-chain failure cleanup must start the pending countdown before returning idle.'
}

$wake = $voice.IndexOf('case VOICE_EVENT_WAKE:')
$wakeEnd = $voice.IndexOf('case VOICE_EVENT_VAD_START:', $wake)
if ($wake -lt 0 -or $wakeEnd -lt 0) { throw 'Wake interruption path is missing.' }
$wakeBlock = $voice.Substring($wake, $wakeEnd - $wake)
if ($wakeBlock -notmatch 'deferred countdown finalized after TTS interruption' -or
    $wakeBlock -notmatch 'timer_service_start_pending_countdowns\(\)') {
    throw 'Interrupting a TTS turn must finalize its pending countdown instead of leaking it into the next turn.'
}

Write-Output 'Deferred voice countdown start contract passed.'
