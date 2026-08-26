$ErrorActionPreference = 'Stop'

$time = Get-Content -Raw 'main/APP/app_time.c'
$weather = Get-Content -Raw 'main/APP/app_weather.c'
$ble = Get-Content -Raw 'main/MID/network/ble_provision.c'
$wifi = Get-Content -Raw 'main/MID/network/wifi_service.c'
$app = Get-Content -Raw 'main/APP/app_manager.c'
$main = Get-Content -Raw 'main/main.c'
$cmake = Get-Content -Raw 'main/CMakeLists.txt'

if ($time -notmatch 'LOCATION_REFRESH_INTERVAL_MS\s+\(12\s*\*\s*60\s*\*\s*60\s*\*\s*1000\)') {
    throw 'location refresh must be scheduled every twelve hours'
}
if ($weather -notmatch 'WEATHER_REFRESH_INTERVAL_MS\s+\(30\s*\*\s*60\s*\*\s*1000\)') {
    throw 'weather refresh must be scheduled every thirty minutes'
}
if ($time -notmatch 'setenv\("TZ"') {
    throw 'time service must apply a runtime timezone instead of only a fixed CST-8 value'
}
if ($ble -notmatch 'BLE_PROVISION_WINDOW_MS\s+\(5\s*\*\s*60\s*\*\s*1000\)') {
    throw 'BLE provisioning must have a five-minute window'
}
if ($ble -notmatch 'startup_gate_enter_deep_sleep\(\)') {
    throw 'BLE window timeout must enter deep sleep'
}
if ($ble -notmatch 'xTimer') {
    throw 'BLE provisioning must have a timeout timer'
}
if ($wifi -notmatch 'ble_provision_open_window\(\)') {
    throw 'Wi-Fi failure and button flow must be able to open BLE provisioning'
}
if ($app -notmatch 'runtime_event_log_init\(\)') {
    throw 'runtime event log must be initialized during application startup'
}
if ($cmake -notmatch 'watchdog_service\.c') {
    throw 'watchdog service must be compiled into the main component'
}
if ($app -notmatch 'runtime_event_log_append\(RUNTIME_EVENT_BOOT') {
    throw 'boot event must be persisted'
}

foreach ($path in @(
    'main/COM/com_status.c', 'main/COM/com_status.h',
    'main/MID/event_bus.c', 'main/MID/event_bus.h',
    'main/Dri/ws2812_matrix.c',
    'main/Dri/ws2812_matrix.h'
)) {
    if (Test-Path $path) { throw "unused legacy file remains: $path" }
}

Write-Output 'plan_completion_test: PASS'

