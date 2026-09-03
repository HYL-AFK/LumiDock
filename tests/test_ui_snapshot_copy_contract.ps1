$ErrorActionPreference = 'Stop'
$ui = Get-Content (Join-Path $PSScriptRoot '..\main\APP\ui_service.c') -Raw
$taskStart = $ui.IndexOf('static void ui_task')
$task = $ui.Substring($taskStart, $ui.IndexOf('esp_err_t ui_service_init') - $taskStart)
if ($task -notmatch 'lumidock_snapshot_t\s+snapshot_copy') { throw 'UI task must copy the snapshot into a local value.' }
if ($task -notmatch 'mid_snapshot_get\(\&snapshot_copy\)') { throw 'UI task must use the snapshot copy API.' }
if ($task -match 'mid_snapshot_read_acquire|mid_snapshot_read_release') { throw 'UI task must not hold the snapshot lock during LVGL rendering.' }
if ($task -match 'snapshot read unavailable; skipping UI data frame') { throw 'UI must not skip an entire frame when snapshot writers are active.' }
Write-Output 'UI snapshot copy contract passed.'
