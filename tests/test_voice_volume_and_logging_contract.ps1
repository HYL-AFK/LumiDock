$ErrorActionPreference = 'Stop'

$driver = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\Dri\i2s_audio_driver.c"
$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

@(
    @{ Content = $driver; Pattern = '#define ES8311_OUTPUT_VOLUME_PERCENT 40\.0f'; Message = 'ES8311 playback must use the requested 40 percent codec-volume target.' },
    @{ Content = $driver; Pattern = 'esp_codec_dev_set_out_vol\(s_codec, s_output_volume_percent\)'; Message = 'The codec must use the runtime output volume value.' },
    @{ Content = $driver; Pattern = 'output %.0f%%, muted'; Message = 'Codec startup log must identify the configured output volume.' },
    @{ Content = $voice; Pattern = 'voice uplink: frames=%u bytes=%u'; Message = 'Voice service must report rate-limited Opus uplink frame and byte totals.' },
    @{ Content = $voice; Pattern = 'cloud TTS: frames=%u bytes=%u'; Message = 'Voice service must report rate-limited cloud TTS frame and byte totals.' },
    @{ Content = $voice; Pattern = 'voice VAD: speech started'; Message = 'Voice service must log the start of captured speech.' },
    @{ Content = $voice; Pattern = 'voice VAD: speech ended'; Message = 'Voice service must log the end of captured speech.' }
) | ForEach-Object {
    if ($_.Content -notmatch $_.Pattern) { throw $_.Message }
}

Write-Output 'Voice volume and traffic logging contract passed.'
