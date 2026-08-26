$ErrorActionPreference = 'Stop'

$pipeline = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\main\MID\voice\xiaozhi_audio_pipeline.c') -Raw

$enqueueBody = [regex]::Match($pipeline, 'esp_err_t xiaozhi_audio_pipeline_enqueue_tts_opus\(.*?\n}\n', 'Singleline').Value
if ($enqueueBody -match 'xSemaphoreTake\(s_pipeline\.playback_lock') {
    throw 'WebSocket TTS enqueue must not wait for PCM playback to finish.'
}
if ($enqueueBody -notmatch 'const uint32_t enqueue_epoch = s_pipeline\.playback_epoch;') {
    throw 'TTS enqueue must snapshot the playback epoch so aborted packets remain discardable.'
}
if ($enqueueBody -notmatch 'packet->epoch = enqueue_epoch;') {
    throw 'Every queued TTS packet must retain the captured playback epoch.'
}

if ($pipeline -notmatch 'audio_playback_service_enqueue_pcm\(AUDIO_PLAYOUT_SOURCE_TTS') {
    throw 'Decoded TTS PCM must be submitted to the shared playout service.'
}
if ($pipeline -match 'i2s_audio_driver_play\(') {
    throw 'The codec task must not write directly to I2S after playout ownership moved to the shared service.'
}

Write-Output 'TTS playback lock contract passed.'
