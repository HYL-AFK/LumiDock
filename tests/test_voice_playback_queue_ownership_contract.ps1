$ErrorActionPreference = 'Stop'

$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"

if ($pipeline -match 'VOICE_AUDIO_CONTROL_DISCARD_PLAYBACK') {
    throw 'The AFE control queue must not carry playback-ringbuffer operations.'
}
if ($pipeline -notmatch 'volatile bool playback_discard_pending' -or
    $pipeline -notmatch 's_pipeline\.playback_discard_pending = true') {
    throw 'Playback invalidation must request discard for the codec owner without touching the TTS queue directly.'
}

$detectStart = $pipeline.IndexOf('static void audio_detect_task(void *arg)')
$decodeStart = $pipeline.IndexOf('static bool decode_one_tts_packet', $detectStart)
if ($detectStart -lt 0 -or $decodeStart -lt 0) {
    throw 'The AFE and codec task boundaries must remain explicit.'
}
$detectTask = $pipeline.Substring($detectStart, $decodeStart - $detectStart)
if ($detectTask -match 'tts_queue|discard_tts_queue') {
    throw 'The AFE detect task must not receive, return, or discard TTS ringbuffer items.'
}

$codecStart = $pipeline.IndexOf('static void voice_codec_task(void *arg)')
$codecEnd = $pipeline.IndexOf('static esp_err_t init_afe', $codecStart)
if ($codecStart -lt 0 -or $codecEnd -lt 0) {
    throw 'The codec task must remain identifiable for ownership verification.'
}
$codecTask = $pipeline.Substring($codecStart, $codecEnd - $codecStart)
if ($codecTask -notmatch 'playback_discard_pending' -or
    $codecTask -notmatch 'discard_tts_queue\(\);') {
    throw 'Only the codec task may drain the TTS queue after an abort or TTS stop.'
}

Write-Output 'Voice playback queue ownership contract passed.'
