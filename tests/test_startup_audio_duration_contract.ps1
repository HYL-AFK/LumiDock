$ErrorActionPreference = 'Stop'

$script = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\scripts\convert_startup_audio.ps1"
$audio = Get-Item -LiteralPath "$PSScriptRoot\..\main\assets\audio\startup_ready_16k_mono_s16le.pcm"

if ($script -notmatch 'atempo=0\.589') {
    throw 'The selected startup sound must be time-stretched to a complete four-second performance, not padded with silence.'
}
if ($script -notmatch 'atrim=duration=4\.0') {
    throw 'Startup audio conversion must produce four seconds of PCM.'
}
if ($audio.Length -ne 128000) {
    throw "Embedded startup PCM must be exactly four seconds at 16 kHz mono s16le; found $($audio.Length) bytes."
}

Write-Output 'Startup audio four-second duration contract passed.'
