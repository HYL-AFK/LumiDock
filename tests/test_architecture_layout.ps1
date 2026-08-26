$ErrorActionPreference = 'Stop'
$cmake = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\CMakeLists.txt"
$oldTree = Get-ChildItem -LiteralPath "$PSScriptRoot\..\main\COM\WiTime" -Force -ErrorAction SilentlyContinue
$source = (Get-ChildItem -LiteralPath "$PSScriptRoot\..\main" -Recurse -File -Include '*.c', '*.h' | ForEach-Object {
    Get-Content -Raw -LiteralPath $_.FullName
}) -join "`n"

if ($oldTree) {
    throw 'The obsolete WiTime directory still exists.'
}
if ($cmake -match 'COM/WiTime|COM/(charging_service|bms_protocol|bms_service|wifi_service|ble_provision_service|time_service|weather_service|single_wire_uart_service)') {
    throw 'CMake still assigns non-common services to COM or references WiTime.'
}
if ($source -match 'WiTime/|"common/') {
    throw 'Source files still reference the pre-migration directory layout.'
}
@('APP/app_time.c', 'APP/app_weather.c', 'COM/app_config.c',
  'Dri/wifi_manager.c', 'MID/network/ble_provision.c', 'MID/network/wifi_service.c') | ForEach-Object {
    if ($cmake -notmatch [regex]::Escape($_)) {
        throw "CMake is missing the migrated source $_."
    }
}

Write-Output 'Architecture layout checks passed.'

