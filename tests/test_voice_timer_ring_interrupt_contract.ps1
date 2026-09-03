$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"
$pipelineH = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.h"
$pipelineC = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$playoutH = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\audio_playback_service.h"
$chimeH = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\timer_chime_audio.h"
$app = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_manager.c"
$cmake = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\CMakeLists.txt"

# Playout source for the chime and the pause-detect pipeline API.
if ($playoutH -notmatch 'AUDIO_PLAYOUT_SOURCE_TIMER_CHIME') {
    throw 'The shared playout must define a TIMER_CHIME source.'
}
if ($pipelineH -notmatch 'xiaozhi_audio_pipeline_pause_detect\(void\)' -or
    $pipelineC -notmatch 'void xiaozhi_audio_pipeline_pause_detect\(void\)' -or
    $pipelineC -notmatch 'post_control\(VOICE_AUDIO_CONTROL_PAUSE_DETECT\)') {
    throw 'The pipeline must expose pause_detect mapping to the PAUSE_DETECT control code.'
}
# Chime keeps WakeNet armed while VAD is paused (ring can be woken away).
if ($pipelineC -notmatch 'if \(s_pipeline\.detect_paused\) continue;') {
    throw 'detect_paused must only suppress VAD while the wake word stays armed.'
}

# Voice service event plumbing: timer events forwarded into the task queue.
if ($voice -notmatch 'VOICE_EVENT_TIMER_RING' -or
    $voice -notmatch 'VOICE_EVENT_TIMER_STOPPED') {
    throw 'The voice event enum must carry TIMER_RING and TIMER_STOPPED.'
}
if ($voice -notmatch 'static void on_timer_event\(timer_service_event_t event, timer_service_slot_t slot\)' -or
    $voice -notmatch 'post_event_value\(VOICE_EVENT_TIMER_RING, \(uint32_t\)slot\)' -or
    $voice -notmatch 'post_event_value\(VOICE_EVENT_TIMER_STOPPED, \(uint32_t\)slot\)') {
    throw 'The timer service callback must forward RING/STOPPED into the voice event queue.'
}
if ($voice -notmatch 'timer_service_init\(on_timer_event\);') {
    throw 'Voice service init must register the timer event callback.'
}

# Chime playback: begin/enqueue/finish through the shared playout.
$chimeStart = $voice.IndexOf('static void begin_timer_chime')
$stopChimeStart = $voice.IndexOf('static void stop_chime', $chimeStart)
$handleStart = $voice.IndexOf('static void handle_event', $stopChimeStart)
if ($chimeStart -lt 0 -or $stopChimeStart -lt 0 -or $handleStart -lt 0) {
    throw 'Chime function boundaries are missing.'
}
$chimeBlock = $voice.Substring($chimeStart, $handleStart - $chimeStart)
if ($chimeBlock -notmatch 'audio_playback_service_begin\(AUDIO_PLAYOUT_SOURCE_TIMER_CHIME' -or
    $chimeBlock -notmatch 'audio_playback_service_enqueue_pcm\(' -or
    $chimeBlock -notmatch 'audio_playback_service_finish\(AUDIO_PLAYOUT_SOURCE_TIMER_CHIME' -or
    $chimeBlock -notmatch 'TIMER_CHIME_PCM_SAMPLES' -or
    $chimeBlock -notmatch 'audio_playback_service_abort\(AUDIO_PLAYOUT_SOURCE_TIMER_CHIME, s_chime_epoch\)') {
    throw 'begin_timer_chime must enqueue the local PCM through the shared playout and stop_chime must abort it.'
}
if ($chimeH -notmatch 'TIMER_CHIME_PCM_SAMPLES' -or
    $chimeH -notmatch 'timer_chime_pcm') {
    throw 'The generated chime resource header must define TIMER_CHIME_PCM_SAMPLES and timer_chime_pcm.'
}

# TIMER_RING handling: interrupt the session, pause detection, play the chime.
$ringCaseStart = $voice.IndexOf('case VOICE_EVENT_TIMER_RING:')
$stopCaseStart = $voice.IndexOf('case VOICE_EVENT_TIMER_STOPPED:', $ringCaseStart)
$handleEnd = $voice.IndexOf('static void voice_session_task', $stopCaseStart)
if ($ringCaseStart -lt 0 -or $stopCaseStart -lt 0 -or $handleEnd -lt 0) {
    throw 'TIMER_RING/TIMER_STOPPED case boundaries are missing.'
}
$ringCase = $voice.Substring($ringCaseStart, $stopCaseStart - $ringCaseStart)
if ($ringCase -notmatch 's_session_active' -or
    $ringCase -notmatch 'timer_interrupt' -or
    $ringCase -notmatch 'close_channel\(false\);' -or
    $ringCase -notmatch 'xiaozhi_audio_pipeline_pause_detect\(\);' -or
    $ringCase -notmatch 'begin_timer_chime\(\);') {
    throw 'TIMER_RING must interrupt the active session, pause detection and play the chime.'
}
$stopCase = $voice.Substring($stopCaseStart, $handleEnd - $stopCaseStart)
if ($stopCase -notmatch 'stop_chime\(\);' -or
    $stopCase -notmatch '!s_session_active \|\| s_state == XIAOZHI_VOICE_WAIT_WAKE' -or
    $stopCase -notmatch 'xiaozhi_audio_pipeline_resume_after_playback\(\);') {
    throw 'TIMER_STOPPED must stop the chime and resume detection unless a fresh session owns it.'
}

# Wake barge-in: the wake word silences a ringing timer before the new turn.
$wakeStart = $voice.IndexOf('case VOICE_EVENT_WAKE:')
$vadStart = $voice.IndexOf('case VOICE_EVENT_VAD_START:', $wakeStart)
$wakeBlock = $voice.Substring($wakeStart, $vadStart - $wakeStart)
if ($wakeBlock -notmatch 'timer_service_is_ringing\(\)' -or
    $wakeBlock -notmatch 'timer_service_stop_ringing\(\);' -or
    $wakeBlock -notmatch 'stop_chime\(\);' -or
    $wakeBlock -notmatch 'xiaozhi_audio_pipeline_resume_after_playback\(\);') {
    throw 'The wake word must stop a ringing timer, abort the chime and resume detection.'
}

# 1 s tick: the session task polls the timer service every second.
$taskStart = $voice.IndexOf('static void voice_session_task')
$taskBlock = $voice.Substring($taskStart)
if ($taskBlock -notmatch 's_last_timer_tick_us >= 1000000LL' -or
    $taskBlock -notmatch 'timer_service_poll\(\);') {
    throw 'The session task must poll the timer service once per second.'
}

# Local IoT command confirmations are intentionally not sent as a second TTS;
# the cloud response is the sole audible acknowledgement.
if ($voice -match 'request_cloud_announce_prompt|IoT announce deferred until the cloud reply drains') {
    throw 'Local IoT commands must not create a second announcement TTS.'
}

# Button short press silences the ring (key stop).
if ($app -notmatch 'timer_service_stop_ringing\(\);') {
    throw 'A short button press must stop the ringing timer.'
}

# Build registration: timer service and chime sources must be compiled.
if ($cmake -notmatch 'MID/system/timer_service\.c' -or
    $cmake -notmatch 'MID/system/timer_chime_audio\.c') {
    throw 'timer_service.c and timer_chime_audio.c must be registered in CMakeLists.'
}

Write-Output 'Voice timer ring interrupt contract passed.'
