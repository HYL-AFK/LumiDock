$ErrorActionPreference = 'Stop'

$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"

@(
    @{ Pattern = 'typedef struct \{\s*uint32_t epoch;\s*size_t length;\s*uint8_t data\[\];\s*\} tts_packet_t;'; Message = 'Each queued TTS packet must carry the playback epoch without adding a large WebSocket-task stack buffer.' },
    @{ Pattern = 'const uint32_t enqueue_epoch = s_pipeline\.playback_epoch;'; Message = 'Inbound cloud TTS must snapshot the active playback epoch before allocation.' },
    @{ Pattern = 'packet->epoch = enqueue_epoch;'; Message = 'Inbound cloud TTS must be stamped with the captured playback epoch.' },
    @{ Pattern = 'packet->epoch != s_pipeline\.playback_epoch'; Message = 'The codec must reject packets invalidated by TTS stop or abort.' },
    @{ Pattern = 'playback_lock'; Message = 'Playback and stop/mute must share one synchronization lock.' },
    @{ Pattern = 'xSemaphoreTake\(s_pipeline\.playback_lock, portMAX_DELAY\)'; Message = 'Codec playback must serialize with TTS stop.' },
    @{ Pattern = 'xSemaphoreGive\(s_pipeline\.playback_lock\)'; Message = 'Playback lock must always be released.' },
    @{ Pattern = 'xRingbufferSendAcquire\(s_pipeline\.tts_queue, \(void \*\*\)&packet, packet_bytes, 0\)'; Message = 'TTS packets must be allocated directly in the PSRAM ringbuffer.' },
    @{ Pattern = 'xRingbufferSendComplete\(s_pipeline\.tts_queue, packet\)'; Message = 'A PSRAM TTS packet must be committed only after its epoch and payload are written.' }
) | ForEach-Object {
    if ($pipeline -notmatch $_.Pattern) { throw $_.Message }
}

$decodeStart = $pipeline.IndexOf('static bool decode_one_tts_packet')
$codecStart = $pipeline.IndexOf('static void voice_codec_task', $decodeStart)
if ($decodeStart -lt 0 -or $codecStart -lt 0) {
    throw 'TTS decode boundary is missing.'
}
$decode = $pipeline.Substring($decodeStart, $codecStart - $decodeStart)
if ($decode -notmatch 'packet->epoch != s_pipeline\.playback_epoch' -or
    $decode -notmatch 'audio_playback_service_enqueue_pcm\(AUDIO_PLAYOUT_SOURCE_TTS') {
    throw 'TTS playback must verify the packet epoch before submitting PCM to the shared output.'
}

Write-Output 'Voice playback epoch contract passed.'
