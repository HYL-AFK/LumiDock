$ErrorActionPreference = 'Stop'

$service = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\startup_audio.c"
$header = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\startup_audio.h"
$driver = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\i2s_audio_driver.c"
$cmake = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\CMakeLists.txt"
$script = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\scripts\convert_startup_audio.ps1"
$asset = Join-Path $PSScriptRoot '..\main\assets\audio\startup_ready_16k_mono_s16le.pcm'

@(
    @{ Content = $header; Pattern = 'startup_audio_play_ready'; Message = 'The startup PCM service needs a one-shot public ready-sound API.' },
    @{ Content = $service; Pattern = 'LUMIDOCK_AUDIO_SAMPLE_RATE_HZ'; Message = 'Startup audio must use the ES8311 native 16 kHz format.' },
    @{ Content = $service; Pattern = 'audio_playback_service_enqueue_pcm'; Message = 'Startup audio must use the shared PCM playout path.' },
    @{ Content = $service; Pattern = 'audio_playback_service_finish'; Message = 'Startup audio must close the shared playout input after queuing.' },
    @{ Content = $service; Pattern = '_binary_startup_ready_16k_mono_s16le_pcm_start'; Message = 'Startup PCM must be embedded from Flash rather than copied into the heap.' },
    @{ Content = $cmake; Pattern = 'assets/audio/startup_ready_16k_mono_s16le\.pcm'; Message = 'The selected boot PCM must be embedded into firmware.' },
    @{ Content = $script; Pattern = '-ar 16000 -ac 1 -f s16le'; Message = 'The offline converter must generate fixed 16 kHz mono s16le PCM.' },
    @{ Content = $driver; Pattern = 'ES8311_OUTPUT_VOLUME_PERCENT 40.0f'; Message = 'Boot sound must use the requested 40 percent ES8311 output volume.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

if (!(Test-Path -LiteralPath $asset)) { throw 'Converted 16 kHz startup PCM resource is missing.' }
$bytes = [System.IO.File]::ReadAllBytes($asset)
if ($bytes.Length -ne 128000 -or ($bytes.Length % 2) -ne 0) {
    throw 'Startup PCM must be exactly four seconds of signed-16-bit 16 kHz mono audio.'
}

Write-Output 'Boot audio contract passed.'
