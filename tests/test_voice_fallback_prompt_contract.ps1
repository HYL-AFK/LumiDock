$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

# The fallback prompt must exist with editable wording and the same
# [device_call] mechanism as the timeout prompt.
if ($voice -notmatch 'request_cloud_fallback_prompt') {
    throw 'Fallback prompt request must exist.'
}
if ($voice -notmatch 's_fallback_phrases') {
    throw 'Fallback prompt wording must live in an editable phrase list.'
}
$fbStart = $voice.IndexOf('static bool request_cloud_fallback_prompt')
$logStart = $voice.IndexOf('static void log_uplink_traffic', $fbStart)
if ($fbStart -lt 0 -or $logStart -lt 0) { throw 'Fallback prompt function boundaries are missing.' }
$fbBlock = $voice.Substring($fbStart, $logStart - $fbStart)
if ($fbBlock -notmatch '\[device_call\]') {
    throw 'The fallback prompt must be wrapped in [device_call] so the server answers with TTS.'
}
if ($fbBlock -notmatch 's_fallback_prompt_turn_id = s_turn_id' -or
    $fbBlock -notmatch 's_state = XIAOZHI_VOICE_EXITING' -or
    $fbBlock -notmatch 's_exit_tts_start_deadline_us = now_us \+ XIAOZHI_EXIT_TTS_START_TIMEOUT_US') {
    throw 'The fallback prompt must open a generated EXITING turn with the three-layer TTS deadline protection.'
}
if ($fbBlock -notmatch 'send_text\(request\)' -or
    $fbBlock -notmatch 's_fallback_prompt_pending = false') {
    throw 'A failed fallback request must return false and roll back its pending markers.'
}

# Every local silent-ending path must ask for a spoken fallback before giving up.
$failureMarkers = @(
    'listen/start send failed; requesting generated fallback prompt',
    'wake server ignore primer failed; requesting generated fallback prompt',
    'unable to start Opus uplink; requesting generated fallback prompt',
    'terminal uplink tail failed[\s\S]{0,120}requesting generated fallback prompt',
    'terminal listen/stop failed[\s\S]{0,120}requesting generated fallback prompt',
    'terminal main write failed[\s\S]{0,120}requesting generated fallback prompt',
    'terminal Opus transport failure[\s\S]{0,120}requesting generated fallback prompt',
    'voice turn ended after Opus transport failure; requesting generated fallback prompt'
)
foreach ($marker in $failureMarkers) {
    if ($voice -notmatch $marker) {
        throw "Silent-ending path must request a fallback prompt: $marker"
    }
}

# The main-write timeout path must also request the fallback prompt.
if ($voice -notmatch 'terminal main write wait timed out[\s\S]{0,400}request_cloud_fallback_prompt') {
    throw 'The terminal main-write timeout must request a fallback prompt before closing.'
}

# A rejected playout must not retry the fallback endlessly: a rejected
# generated prompt goes back to wake-wait without another request.
$ttsStart = $voice.IndexOf('strcmp(state->valuestring, "start") == 0')
$ttsStop = $voice.IndexOf('strcmp(state->valuestring, "stop") == 0', $ttsStart)
if ($ttsStart -lt 0 -or $ttsStop -lt 0) { throw 'TTS state handling boundaries are missing.' }
$ttsStartBlock = $voice.Substring($ttsStart, $ttsStop - $ttsStart)
if ($ttsStartBlock -notmatch 'rejected_generated_prompt' -or
    $ttsStartBlock -notmatch 'cloud TTS rejected[\s\S]{0,120}requesting generated fallback prompt') {
    throw 'A rejected cloud TTS must request a fallback prompt once, and never retry a rejected generated prompt.'
}

# A drained fallback prompt must re-arm the 15 second follow-up window.
if ($voice -notmatch 'fallback prompt PCM drained; follow-up window=15s') {
    throw 'Fallback prompt must re-arm the 15 second follow-up window after playback.'
}

# Cleanup symmetry: close_channel, wake barge-in, and sibling prompt requests
# must all clear the fallback markers.
$closeStart = $voice.IndexOf('static void close_channel')
$publishStart = $voice.IndexOf('static bool send_uplink_packet', $closeStart)
if ($closeStart -lt 0 -or $publishStart -lt 0) { throw 'close_channel boundaries are missing.' }
$closeBlock = $voice.Substring($closeStart, $publishStart - $closeStart)
if ($closeBlock -notmatch 's_fallback_prompt_pending = false' -or
    $closeBlock -notmatch 's_playing_fallback_prompt = false' -or
    $closeBlock -notmatch 's_fallback_prompt_turn_id = 0U') {
    throw 'close_channel must clear the fallback prompt markers.'
}
if ($voice -notmatch 'xiaozhi_audio_pipeline_abort_playback\(\);[\s\S]{0,1200}?s_fallback_prompt_pending = false;') {
    throw 'Wake barge-in must clear fallback prompt bookkeeping.'
}
$exitStart = $voice.IndexOf('static bool request_cloud_exit_prompt')
$timeoutStart = $voice.IndexOf('static bool request_cloud_timeout_prompt', $exitStart)
if ($exitStart -lt 0 -or $timeoutStart -lt 0) { throw 'Prompt function boundaries are missing.' }
$exitBlock = $voice.Substring($exitStart, $timeoutStart - $exitStart)
if ($exitBlock -notmatch 's_fallback_prompt_pending = false;[\s\S]{0,80}s_fallback_prompt_turn_id = 0U;') {
    throw 'request_cloud_exit_prompt must clear fallback prompt markers.'
}
$timeoutStart2 = $voice.IndexOf('static bool request_cloud_timeout_prompt')
$timeoutBlock = $voice.Substring($timeoutStart2, $fbStart - $timeoutStart2)
if ($timeoutBlock -notmatch 's_fallback_prompt_pending = false;[\s\S]{0,80}s_fallback_prompt_turn_id = 0U;') {
    throw 'request_cloud_timeout_prompt must clear fallback prompt markers.'
}

Write-Output 'Voice fallback prompt contract passed.'
