$ErrorActionPreference = 'Stop'

$voice = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\main\MID\voice\xiaozhi_voice_service.c') -Raw
$pipeline = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\main\MID\voice\xiaozhi_audio_pipeline.c') -Raw
$playout = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\main\MID\system\audio_playback_service.c') -Raw

foreach ($required in @('s_tts_start_us', 'tts first binary latency', 'boot greeting tts start received', 'boot greeting first decodable PCM latency', 'TTS short binary packet', 's_tts_short_packet_count', 'TTS packet arrival:')) {
    if ($voice -notmatch [regex]::Escape($required)) {
        throw "Voice service lacks required downlink diagnostic: $required"
    }
}
foreach ($required in @('TTS decode timing:', 'playout_enqueue_us=', 'tts_packet_boundary_mismatches', 'tts_decode_failure_count', 'TTS decode failure count=')) {
    if ($pipeline -notmatch [regex]::Escape($required)) {
        throw "Audio pipeline lacks required downlink diagnostic: $required"
    }
}
if ($playout -notmatch 'PCM playout write:.*write_us=') {
    throw 'The shared playout task must log actual I2S write duration.'
}
if ($pipeline -notmatch 'tts_queue_full_count' -or $pipeline -notmatch 'TTS queue full: count=') {
    throw 'TTS playback-queue saturation must have its own counter and diagnostic.'
}
if ($voice -notmatch 's_voice_event_queue_full_count' -or $voice -notmatch 's_tts_packet_drop_count') {
    throw 'Voice-service queue overflow and TTS drop counters must be retained for diagnosis.'
}

Write-Output 'TTS downlink diagnostics contract passed.'
