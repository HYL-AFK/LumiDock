$ErrorActionPreference = 'Stop'

$codecDev = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\managed_components\espressif__esp_codec_dev\esp_codec_dev.c') -Raw
$es8311 = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\managed_components\espressif__esp_codec_dev\device\es8311\es8311.c') -Raw
$driver = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\main\Dri\i2s_audio_driver.c') -Raw

if ($codecDev -notmatch 'dev->disable_when_closed = true;(?s:.*?)dev->muted = true;(?s:.*?)return \(esp_codec_dev_handle_t\) dev;') {
    throw 'Codec-device construction must default the output state to muted before ES8311 open.'
}

$enableBody = [regex]::Match($es8311, 'static int es8311_enable\(.*?\n}\n\nstatic int es8311_set_reg', 'Singleline').Value
if ($enableBody -match 'es8311_set_mute\(h, false\)') {
    throw 'ES8311 enable must not automatically unmute the DAC during startup.'
}
if ($enableBody -notmatch 'es8311_set_mute\(h, true\)') {
    throw 'ES8311 enable must retain DAC mute until a PCM playback request explicitly unmutes it.'
}

if ($driver -notmatch 'i2s_audio_driver_start_output\s*\(' -or
    $driver -notmatch 'set_codec_output_mute_with_retry\(false\)') {
    throw 'PCM playback must remain the sole path that unmutes the ES8311 DAC through the explicit start API.'
}

Write-Output 'ES8311 boot mute contract passed.'
