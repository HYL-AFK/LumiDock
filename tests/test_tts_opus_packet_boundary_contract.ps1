$ErrorActionPreference = 'Stop'

$pipeline = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\main\MID\voice\xiaozhi_audio_pipeline.c') -Raw

if ($pipeline -notmatch '\.self_delimited = false') {
    throw 'XiaoZhi downlink must continue using bare, non-self-delimited Opus packets.'
}
if ($pipeline -notmatch '#define XIAOZHI_TTS_QUEUE_BYTES \(24U \* 1024U\)' -or
    $pipeline -notmatch '#define XIAOZHI_TTS_PACKET_MAX_BYTES 2048U') {
    throw 'The existing 24 KiB PSRAM queue and 2048-byte TTS packet cap must remain unchanged.'
}

$decodeBody = [regex]::Match($pipeline, 'static bool decode_one_tts_packet\(.*?\n}\n\nstatic void voice_codec_task', 'Singleline').Value
if ([regex]::Matches($decodeBody, 'esp_opus_dec_decode\(').Count -ne 1) {
    throw 'Each complete WebSocket TTS packet must call the raw Opus decoder exactly once.'
}
if ($decodeBody -match 'while \(input\.len > 0U\)') {
    throw 'A bare Opus packet must not be decoded repeatedly from any residual input bytes.'
}
if ($decodeBody -notmatch 'input\.consumed != packet->length') {
    throw 'A decoder packet-boundary mismatch must be diagnosed without decoding the remainder.'
}
if ($decodeBody -notmatch 'TTS packet boundary mismatch') {
    throw 'Decoder packet-boundary mismatches must have an explicit diagnostic.'
}

Write-Output 'TTS Opus packet-boundary contract passed.'
