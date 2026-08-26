$ErrorActionPreference = 'Stop'

$main = Get-Content -Raw 'main/main.c'
if ($main -notmatch '#define\s+LUMIDOCK_NETWORK_TEST_MODE\s+1') {
    throw 'network test mode must be enabled while the button path is under test'
}
if ($main -notmatch '#if\s+LUMIDOCK_NETWORK_TEST_MODE') {
    throw 'network test mode must bypass the button startup gate'
}
if ($main -notmatch 'app_manager_start\(\)') {
    throw 'network test mode must still start the application services'
}

Write-Output 'network_test_mode_test: PASS'

