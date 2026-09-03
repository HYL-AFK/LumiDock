$ErrorActionPreference = 'Stop'

$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

function Get-Block($content, $startNeedle, $endNeedle) {
    $start = $content.IndexOf($startNeedle)
    if ($start -lt 0) { throw "Missing block start: $startNeedle" }
    $end = $content.IndexOf($endNeedle, $start)
    if ($end -lt 0) { throw "Missing block end: $endNeedle" }
    return $content.Substring($start, $end - $start)
}

# The detect task must keep the wake word armed during playback so the user
# can abort a long answer, while VAD stays paused to keep the speaker echo
# from self-triggering speech capture.
$detectTask = Get-Block $pipeline 'static void audio_detect_task' 'static void playout_event'
if ($detectTask -notmatch 'WAKENET_DETECTED[\s\S]*?wake_cb\(s_pipeline\.callback_context\)') {
    throw 'Wake word callback must fire from the detect task.'
}
if ($detectTask -match 'WAKENET_DETECTED[\s\S]*?if \(s_pipeline\.detect_paused\) continue;[\s\S]*?wake_cb\(s_pipeline\.callback_context\)') {
    throw 'Wake word detection must not be gated behind detect_paused during playback.'
}
if ($detectTask -notmatch 'wake_cb\(s_pipeline\.callback_context\);[\s\S]*?if \(s_pipeline\.detect_paused\) continue;[\s\S]*?vad_cb\(') {
    throw 'VAD must remain paused during playback to avoid echo self-triggering.'
}

# The WAKE abort path must resume AFE detection after aborting playback,
# otherwise the follow-up conversation stays deaf forever.
$handleEvent = Get-Block $voice 'static void handle_event' 'static void voice_session_task'
if ($handleEvent -notmatch 'xiaozhi_audio_pipeline_abort_playback\(\);[\s\S]*?xiaozhi_audio_pipeline_resume_after_playback\(\);') {
    throw 'Wake abort must resume AFE detection after aborting playback.'
}
if ($handleEvent -notmatch 's_playing_timeout_prompt = false;[\s\S]*?s_exit_tts_playback_deadline_us = 0;') {
    throw 'Wake abort must clear prompt bookkeeping so stale deadlines cannot close the fresh turn.'
}

# The timeout prompt flow: cloud TTS, playback, then a fresh 15 second
# follow-up window instead of a session close.
if ($voice -notmatch 'request_cloud_timeout_prompt') {
    throw 'Timeout prompt request must exist.'
}
if ($voice -notmatch 'timeout prompt PCM drained; follow-up window=15s') {
    throw 'Timeout prompt must re-arm the 15 second follow-up window after playback.'
}
if ($voice -notmatch 's_timeout_phrases') {
    throw 'Timeout prompt wording must live in an editable phrase list.'
}

Write-Output 'Voice wake interrupt contract passed.'
