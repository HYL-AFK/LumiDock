$ErrorActionPreference = 'Stop'

$main = Get-Content -Raw 'main/main.c'
$gate = Get-Content -Raw 'main/APP/startup_gate.c'
$header = Get-Content -Raw 'main/APP/startup_gate.h'
$cmake = Get-Content -Raw 'main/CMakeLists.txt'

if ($main -notmatch 'startup_gate_wait_for_long_press\(\)') {
    throw 'main must wait for the confirmed button long-press startup gate'
}

if ($gate -notmatch 'esp_sleep_enable_ext0_wakeup') {
    throw 'startup gate must configure GPIO wake from deep sleep'
}

if ($gate -notmatch 'STARTUP_GATE_HOLD_MS\s+3000') {
    throw 'startup gate hold time must be three seconds'
}

if ($gate -notmatch 'esp_deep_sleep_start') {
    throw 'startup gate must enter deep sleep when the button is not held'
}

if ($header -notmatch 'startup_gate_enter_deep_sleep\(void\)') {
    throw 'startup gate must expose a deep-sleep entry API for BLE timeout handling'
}

if ($cmake -notmatch 'APP/startup_gate\.c') {
    throw 'startup gate source must be part of the main component'
}

Write-Output 'startup_sleep_gate_test: PASS'

