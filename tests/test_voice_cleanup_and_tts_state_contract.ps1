$ErrorActionPreference = 'Stop'

$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

@(
    @{ Content = $pipeline; Pattern = 'vTaskDeleteWithCaps\(s_pipeline\.codec_task\)'; Message = 'The PSRAM codec task must be released with the matching WithCaps API.' },
    @{ Content = $pipeline; Pattern = 'vTaskDeleteWithCaps\(s_pipeline\.detect_task\)'; Message = 'The PSRAM detect task must be released with the matching WithCaps API.' },
    @{ Content = $pipeline; Pattern = 'vTaskDeleteWithCaps\(s_pipeline\.feed_task\)'; Message = 'The PSRAM feed task must be released with the matching WithCaps API.' },
    @{ Content = $pipeline; Pattern = 'vQueueDeleteWithCaps\(s_pipeline\.pcm_frames\)'; Message = 'The PSRAM PCM queue must be released with the matching WithCaps API.' },
    @{ Content = $pipeline; Pattern = 'vRingbufferDeleteWithCaps\(s_pipeline\.tts_queue\)'; Message = 'The PSRAM TTS ringbuffer must be released with the matching WithCaps API.' },
    @{ Content = $pipeline; Pattern = 'uint8_t \*codec_opus;' ; Message = 'The codec Opus buffer must have pipeline-owned lifetime.' },
    @{ Content = $pipeline; Pattern = 'uint8_t \*decode_pcm;' ; Message = 'The decoder PCM buffer must have pipeline-owned lifetime.' },
    @{ Content = $pipeline; Pattern = 'pcm_frame_t \*codec_frame;' ; Message = 'The codec PCM frame must have pipeline-owned lifetime.' },
    @{ Content = $pipeline; Pattern = 'heap_caps_free\(s_pipeline\.codec_opus\)' ; Message = 'Pipeline cleanup must release the owned Opus buffer.' },
    @{ Content = $pipeline; Pattern = 'heap_caps_free\(s_pipeline\.decode_pcm\)' ; Message = 'Pipeline cleanup must release the owned decoder PCM buffer.' },
    @{ Content = $pipeline; Pattern = 'heap_caps_free\(s_pipeline\.codec_frame\)' ; Message = 'Pipeline cleanup must release the owned codec frame.' },
    @{ Content = $pipeline; Pattern = 'tts decode format: stream_rate=%u channels=%u bits=%u frame=%u pcm=%u' ; Message = 'The first TTS decode must log its negotiated stream format for hardware verification.' },
    @{ Content = $voice; Pattern = 'vTaskDeleteWithCaps\(s_session_task\)'; Message = 'The PSRAM voice session task must be released with the matching WithCaps API.' },
    @{ Content = $voice; Pattern = 'vRingbufferDeleteWithCaps\(s_uplink_queue\)'; Message = 'The PSRAM uplink ringbuffer must be released with the matching WithCaps API.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

$codecStart = $pipeline.IndexOf('static void voice_codec_task(void *arg)')
$afeStart = $pipeline.IndexOf('static esp_err_t init_afe', $codecStart)
$codecTask = $pipeline.Substring($codecStart, $afeStart - $codecStart)
if ($codecTask -match 'vTaskDelete\(NULL\)') {
    throw 'A WithCaps codec task must not self-delete with the ordinary FreeRTOS API.'
}
if ($codecTask -notmatch 'vTaskSuspend\(NULL\)') {
    throw 'A failed codec task must stay suspended until the owning cleanup path releases its WithCaps allocation.'
}
$feedStart = $pipeline.IndexOf('static void audio_feed_task(void *arg)')
$detectStart = $pipeline.IndexOf('static void audio_detect_task(void *arg)', $feedStart)
$feedTask = $pipeline.Substring($feedStart, $detectStart - $feedStart)
if ($feedTask -match 'vTaskDelete\(NULL\)') {
    throw 'A WithCaps feed task must not self-delete with the ordinary FreeRTOS API.'
}
if ($feedTask -notmatch 'vTaskSuspend\(NULL\)') {
    throw 'A failed feed task must stay suspended until the owning cleanup path releases its WithCaps allocation.'
}

$binaryStart = $voice.IndexOf('case VOICE_EVENT_WSS_BINARY:')
$taskStart = $voice.IndexOf('static void voice_session_task', $binaryStart)
if ($binaryStart -lt 0 -or $taskStart -lt 0) {
    throw 'The WSS binary event handler boundary is missing.'
}
$binaryHandler = $voice.Substring($binaryStart, $taskStart - $binaryStart)
if ($binaryHandler -notmatch 's_discard_tts_stream \|\|\s*\(s_state\s*!=\s*XIAOZHI_VOICE_TTS_BUFFERING\s*&&\s*s_state\s*!=\s*XIAOZHI_VOICE_SPEAKING\)\s*\|\|' -or
    $binaryHandler -notmatch 'late TTS packet discarded outside accepted speaking turn') {
    throw 'Late TTS packets must be dropped after tts stop instead of being enqueued for playback.'
}

Write-Output 'Voice cleanup and TTS-state contract passed.'
