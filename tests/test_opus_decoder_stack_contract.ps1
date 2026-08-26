$ErrorActionPreference = 'Stop'

$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"

@(
    @{ Content = $pipeline; Pattern = 'voice_codec_task,\s+"voice_codec",\s+VOICE_CODEC_STACK_BYTES,\s+NULL,\s+4,\s+&s_pipeline\.codec_task,\s+0,\s+MALLOC_CAP_SPIRAM \| MALLOC_CAP_8BIT'; Message = 'The unified Opus codec task must use the explicit PSRAM stack budget.' },
    @{ Content = $pipeline; Pattern = 'esp_opus_dec_decode\(s_pipeline\.decoder'; Message = 'The codec task must use the initialized Opus decoder handle.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

Write-Output 'Opus decoder stack contract passed.'
