$ErrorActionPreference = 'Stop'

$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"
$manager = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_manager.c"

@(
    @{ Content = $pipeline; Pattern = 'voice_codec_task'; Message = 'The unified codec task must own the encoder lifecycle.' },
    @{ Content = $pipeline; Pattern = 'voice codec ready'; Message = 'The codec task must report successful initialization.' },
    @{ Content = $manager; Pattern = 'voice startup waiting for initial network requests to settle'; Message = 'Audio startup must wait for the initial UI network burst, without delaying it.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

$setStreamingStart = $pipeline.IndexOf('esp_err_t xiaozhi_audio_pipeline_set_streaming(bool enabled)')
$setStreamingEnd = $pipeline.IndexOf('void xiaozhi_audio_pipeline_reset_wake_word(void)', $setStreamingStart)
$setStreaming = $pipeline.Substring($setStreamingStart, $setStreamingEnd - $setStreamingStart)
if ($setStreaming -match 'ensure_encoder_task\(') {
    throw 'A wake or follow-up must not create the encoder task at runtime.'
}

foreach ($functionName in @('xiaozhi_audio_pipeline_reset_wake_word', 'xiaozhi_audio_pipeline_arm_followup')) {
    $start = $pipeline.IndexOf("void $functionName(void)")
    $end = $pipeline.IndexOf("`n}", $start)
    if ($start -lt 0 -or $end -lt 0) { throw "Missing $functionName." }
    if ($pipeline.Substring($start, $end - $start) -match 'reset_buffer') {
        throw "$functionName must not call AFE reset_buffer from the voice state-machine task."
    }
}

Write-Output 'Voice lifetime and AFE ownership contract passed.'
