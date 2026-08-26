$ErrorActionPreference = 'Stop'

$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"

@(
    @{ Pattern = 'voice_codec_task'; Message = 'A single voice codec task must own Opus encoding and decoding.' },
    @{ Pattern = 'PCM_FRAME_QUEUE_DEPTH'; Message = 'PCM handoff must use a bounded complete-frame queue.' },
    @{ Pattern = 'VOICE_AUDIO_CONTROL_START_STREAM'; Message = 'AFE state changes must be delivered through explicit control events.' },
    @{ Pattern = 'playback_discard_pending'; Message = 'Playback queue invalidation must be requested for the codec task.' },
    @{ Pattern = 'static void audio_detect_task[\s\S]*?apply_audio_controls\(\)'; Message = 'The AFE fetch owner must apply queued audio controls.' },
    @{ Pattern = 'xTaskCreatePinnedToCoreWithCaps\(voice_codec_task, "voice_codec", VOICE_CODEC_STACK_BYTES, NULL, 4,\s*&s_pipeline\.codec_task, 0,\s*MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT\)'; Message = 'The unified codec task must use the explicit PSRAM stack budget.' },
    @{ Pattern = 'pcm frame queued'; Message = 'The frame handoff boundary must be observable in serial logs.' },
    @{ Pattern = 'Opus encode begin'; Message = 'The codec boundary must be observable in serial logs.' },
    @{ Pattern = 'Opus packet queued'; Message = 'The independent Opus packet handoff must be observable in serial logs.' }
) | ForEach-Object {
    if ($pipeline -notmatch $_.Pattern) { throw $_.Message }
}

@('xRingbufferReceiveUpTo\(', 'audio_encode_task', 'audio_decode_task', 'discard_pcm_pending') | ForEach-Object {
    if ($pipeline -match $_) { throw "Legacy byte-pipeline token must be absent: $_" }
}

Write-Output 'Single-codec voice pipeline contract passed.'
