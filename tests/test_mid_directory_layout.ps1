$ErrorActionPreference = 'Stop'

$root = Join-Path $PSScriptRoot '..\main'
$expectedFiles = @(
    'MID\network\wifi_service.c',
    'MID\network\ble_provision.c',
    'MID\network\ble_provision_service.c',
    'MID\network\blufi_security.c',
    'MID\network\time_service.c',
    'MID\network\weather_service.c',
    'MID\protocol\bms_protocol.c',
    'MID\protocol\bms_service.c',
    'MID\protocol\stc_protocol.c',
    'MID\protocol\stc_service.c',
    'MID\voice\xiaozhi_activation_service.c',
    'MID\voice\xiaozhi_voice_service.c',
    'MID\voice\xiaozhi_audio_pipeline.c',
    'MID\system\watchdog_service.c',
    'MID\mid_snapshot.c',
    'MID\mid_snapshot.h'
)

$expectedFiles | ForEach-Object {
    if (-not (Test-Path -LiteralPath (Join-Path $root $_))) {
        throw "MID file must be organized at $_"
    }
}

$cmake = Get-Content -Raw -LiteralPath (Join-Path $root 'CMakeLists.txt')
@('MID/network', 'MID/protocol', 'MID/voice', 'MID/system') | ForEach-Object {
    if ($cmake -notmatch [regex]::Escape($_)) {
        throw "CMake must include $_"
    }
}

Write-Output 'MID directory layout contract passed.'

