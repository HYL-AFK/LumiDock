$ErrorActionPreference = 'Stop'

$pipeline = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_audio_pipeline.c"

if ($pipeline -notmatch 'config->vad_min_noise_ms = 1500;') {
    throw 'Voice VAD must retain a 1.5-second silence threshold so natural phrase pauses are not cut off.'
}

Write-Output 'Voice silence-hold contract passed.'
