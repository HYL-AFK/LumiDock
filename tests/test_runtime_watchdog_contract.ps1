$ErrorActionPreference = 'Stop'

$manager = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\APP\app_manager.c"
$watchdog = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\watchdog_service.c"
$runtimeLog = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\COM\runtime_event_log.c"
$runtimeHeader = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\COM\runtime_event_log.h"

$audioStart = $manager.IndexOf('static void audio_startup_task(void *arg)')
$audioEnd = $manager.IndexOf('esp_err_t app_manager_start(void)', $audioStart)
if ($audioStart -lt 0 -or $audioEnd -lt 0) {
    throw 'Cannot locate the one-shot audio startup task.'
}
$audioBody = $manager.Substring($audioStart, $audioEnd - $audioStart)

if ($audioBody -match 'watchdog_service_subscribe_current_task|watchdog_service_feed') {
    throw 'The one-shot audio startup task must not subscribe to the 3-second application watchdog.'
}
if ($manager -match '#if !LUMIDOCK_NETWORK_UI_TEST_MODE\s+if \(\(err = watchdog_service_init\(\)\)') {
    throw 'The watchdog must run in the current bench profile after task lifecycle auditing.'
}
if ($watchdog -notmatch 'baseline_us' -or
    $watchdog -notmatch 'slot->last_feed_us = baseline_us' -or
    $watchdog -notmatch 'slot->stale_reported = false') {
    throw 'Resuming business watchdog checks must reset all task heartbeat baselines.'
}
if ($runtimeHeader -notmatch 'runtime_event_log_dump_recent' -or
    $runtimeLog -notmatch 'void runtime_event_log_dump_recent') {
    throw 'Persistent events must expose a serial dump API.'
}
if ($manager -notmatch 'runtime_event_log_dump_recent\(16U\)') {
    throw 'Boot must emit the recent persistent event history to COM6.'
}

$watchdogHeader = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\watchdog_service.h"
$statusLed = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\status_led_service.c"

# A late heartbeat on a saturated core is a scheduling symptom, not a hang.
# Latching it used to kill the hardware watchdog and reboot the whole device in
# the middle of radio playback, so reboot is now opt-in per task.
if ($watchdog -notmatch 'if \(!slot->fatal\) continue;') {
    throw 'A late heartbeat must not latch a reboot unless the task opted in.'
}
if ($watchdogHeader -notmatch 'watchdog_service_set_current_task_fatal') {
    throw 'Tasks must be able to opt into reboot-on-stale-heartbeat.'
}
if ($statusLed -notmatch 'watchdog_service_set_current_task_fatal\(false\)') {
    throw 'The decorative LED strip must stay out of the reboot path.'
}
if ($statusLed -notmatch '"status_led", 3072, NULL, 3, NULL, 1,') {
    throw 'The status LED task must outrank the priority-2 core-1 cluster it shares.'
}

Write-Output 'Runtime log and watchdog contract passed.'
