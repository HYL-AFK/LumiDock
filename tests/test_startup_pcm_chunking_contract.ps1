$ErrorActionPreference = 'Stop'

$startupAudio = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\main\APP\startup_audio.c') -Raw

if ($startupAudio -notmatch '#define STARTUP_PCM_WRITE_CHUNK_BYTES 960U') {
    throw 'Startup PCM must use bounded 30 ms I2S write chunks.'
}
if ($startupAudio -notmatch 'while \(offset < pcm_bytes\)') {
    throw 'Startup PCM must stream its embedded data in multiple bounded writes.'
}
if ($startupAudio -notmatch 'const size_t chunk_bytes = \(pcm_bytes - offset\) > STARTUP_PCM_WRITE_CHUNK_BYTES') {
    throw 'Startup PCM writes must cap every transfer at the configured chunk size.'
}
if ($startupAudio -match 'i2s_audio_driver_play\(startup_ready_pcm_start, pcm_bytes, NULL\)') {
    throw 'Startup PCM must not submit the entire four-second resource in one I2S write.'
}

Write-Output 'Startup PCM chunking contract passed.'
