$ErrorActionPreference = 'Stop'

$voice = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\voice\xiaozhi_voice_service.c"

if ($voice -notmatch 'xiaozhi voice link build: single-session-manual-v1') {
    throw 'The voice service must expose the single-session-manual-v1 firmware marker.'
}

Write-Output 'Voice build marker contract passed.'
