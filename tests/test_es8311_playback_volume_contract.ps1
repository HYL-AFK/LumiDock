$ErrorActionPreference = 'Stop'

$driver = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\main\Dri\i2s_audio_driver.c') -Raw

if ($driver -notmatch '#define ES8311_OUTPUT_VOLUME_PERCENT 40\.0f') {
    throw 'ES8311 playback must retain the configured 40 percent codec volume.'
}

if ($driver -notmatch 'ES8311 PCM write failed: code=%d') {
    throw 'ES8311 playback must log a failed PCM write rather than silently discarding it.'
}

Write-Output 'ES8311 playback volume contract passed.'
