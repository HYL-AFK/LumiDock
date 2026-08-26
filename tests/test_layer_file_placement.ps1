$ErrorActionPreference = 'Stop'
$root = Join-Path $PSScriptRoot '..\main'
$cmake = Get-Content -Raw -LiteralPath (Join-Path $root 'CMakeLists.txt')

if (Test-Path -LiteralPath (Join-Path $root 'COM\WiTime')) {
    throw 'The legacy WiTime directory must not remain under COM.'
}
@(
    'APP\app_time.c', 'APP\app_weather.c',
    'COM\app_config.c', 'COM\runtime_event_log.c',
    'Dri\wifi_manager.c', 'Dri\internet_health_policy.c',
    'MID\network\ble_provision.c', 'MID\network\blufi_security.c',
    'MID\network\wifi_service.c', 'MID\network\time_service.c', 'MID\network\weather_service.c'
) | ForEach-Object {
    if (-not (Test-Path -LiteralPath (Join-Path $root $_))) {
        throw "Layered source file is missing: $_"
    }
}
if ($cmake -match 'WiTime' -or $cmake -match 'COM/wifi_service.c') {
    throw 'CMake must use the flattened APP, COM, Dri, and MID paths.'
}

Write-Output 'Layered file placement checks passed.'

