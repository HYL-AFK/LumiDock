$ErrorActionPreference = 'Stop'

$header = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.h"
$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

if ($header -notmatch 'xiaozhi_vad_state_cb_t\)\(bool speech, uint32_t recent_peak' -or
    $header -notmatch 'xiaozhi_capture_complete_cb_t') {
    throw 'The audio pipeline must report VAD energy and completed-capture diagnostics to the voice state machine.'
}
if ($pipeline -notmatch 'config->vad_min_noise_ms = 1500;' -or
    $pipeline -notmatch 's_pipeline\.vad_cb\(result->vad_state == VAD_SPEECH, recent_peak' -or
    $pipeline -notmatch 's_pipeline\.capture_complete_cb\(capture_rms, s_pipeline\.capture_peak' -or
    $pipeline -notmatch '#define VAD_DIRECT_REPLY_PEAK 1500U' -or
    $pipeline -notmatch 'bool vad_high_energy_notified;' -or
    $pipeline -notmatch '!s_pipeline\.vad_high_energy_notified\s*&&\s*recent_peak >= VAD_DIRECT_REPLY_PEAK') {
    throw 'The pipeline must retain a 1.5 s VAD tail and report peak-tagged VAD/capture events.'
}

foreach ($pattern in @(
    '#define XIAOZHI_POST_PLAYBACK_SILENCE_RESET_PEAK 500U',
    '#define XIAOZHI_DIRECT_REPLY_BYPASS_PEAK 1500U',
    '#define XIAOZHI_CAPTURE_LOW_LEVEL_RMS 200U',
    'VOICE_EVENT_CAPTURE_FINISHED',
    'static void handle_capture_low_level_if_ready\(void\)',
    'capture low level; bypassing cloud response wait',
    'request_cloud_timeout_prompt\(\)'
)) {
    if ($voice -notmatch $pattern) {
        throw "Missing post-TTS capture protection: $pattern"
    }
}

$vadStart = $voice.IndexOf('case VOICE_EVENT_VAD_START:')
$vadEnd = $voice.IndexOf('case VOICE_EVENT_VAD_END:', $vadStart)
if ($vadStart -lt 0 -or $vadEnd -lt 0) { throw 'VAD state-machine boundaries are missing.' }
$vadBlock = $voice.Substring($vadStart, $vadEnd - $vadStart)
if ($vadBlock -notmatch 'const uint32_t recent_peak = event->detail;' -or
    $vadBlock -notmatch 'consume_post_playback_vad_start\(recent_peak\)') {
    throw 'VAD starts must carry the pipeline energy measurement into the post-TTS gate.'
}

$gateStart = $voice.IndexOf('static bool consume_post_playback_vad_start')
$gateEnd = $voice.IndexOf('static void finish_wake_greeting_after_playback', $gateStart)
if ($gateStart -lt 0 -or $gateEnd -lt 0) { throw 'Post-TTS VAD gate helper is missing.' }
$gateBlock = $voice.Substring($gateStart, $gateEnd - $gateStart)
if ($gateBlock -notmatch 'recent_peak < XIAOZHI_DIRECT_REPLY_BYPASS_PEAK' -or
    $gateBlock -notmatch 'recent_peak < XIAOZHI_POST_PLAYBACK_SILENCE_RESET_PEAK' -or
    $gateBlock -notmatch 'start_listen\(true\)' -or
    $gateBlock -notmatch 'start_followup_listen\(\)') {
    throw 'High-energy direct replies must bypass silence waiting while only >=500 peaks extend the echo guard.'
}

Write-Output 'Post-TTS capture gate contract passed.'
