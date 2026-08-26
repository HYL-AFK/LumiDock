$ErrorActionPreference = 'Stop'

$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"

if ($pipeline -notmatch 'config->vad_min_noise_ms = 1000;') {
    throw 'Voice VAD must use the reference project 1-second silence threshold before ending the uplink.'
}

Write-Output 'Voice silence-hold contract passed.'
