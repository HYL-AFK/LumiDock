$ErrorActionPreference = 'Stop'

$svc = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\timer_service.c"
$hdr = Get-Content -Raw -LiteralPath "$PSScriptRoot\..\main\MID\system\timer_service.h"

# Slot limits: one one-shot alarm and two countdowns, exactly as agreed.
if ($hdr -notmatch 'TIMER_SERVICE_MAX_ALARMS 1U' -or
    $hdr -notmatch 'TIMER_SERVICE_MAX_COUNTDOWNS 2U') {
    throw 'Timer slot limits must be 1 alarm + 2 countdowns.'
}

# Ring behavior: 2 s chime heartbeat, 30 s auto-stop, 30-tick NVS flush.
if ($hdr -notmatch 'TIMER_SERVICE_RING_HEARTBEAT_MS 2000U' -or
    $hdr -notmatch 'TIMER_SERVICE_RING_TIMEOUT_MS 30000U' -or
    $hdr -notmatch 'TIMER_SERVICE_COUNTDOWN_PERSIST_TICKS 30U') {
    throw 'Ring heartbeat (2 s), auto-stop (30 s) and NVS flush ticks (30) must be present.'
}

# NVS persistence: dedicated namespace with alarm/countdown blob keys.
if ($svc -notmatch 'TIMER_NVS_NAMESPACE "lumidock_timer"' -or
    $svc -notmatch 'TIMER_NVS_KEY_ALARMS "alarms"' -or
    $svc -notmatch 'TIMER_NVS_KEY_COUNTDOWNS "countdowns"' -or
    $svc -notmatch 'nvs_set_blob' -or
    $svc -notmatch 'nvs_get_blob') {
    throw 'Alarms/countdowns must persist through NVS blobs in a dedicated namespace.'
}

# The stopwatch must be in-memory only: no stopwatch state may reach NVS.
if ($svc -notmatch 'Stopwatch: in-memory only, resets on power loss') {
    throw 'The stopwatch must be documented as in-memory only.'
}
$persistAlarmStart = $svc.IndexOf('static esp_err_t persist_alarms')
$persistCountStart = $svc.IndexOf('static esp_err_t persist_countdowns')
$loadStart = $svc.IndexOf('static void load_persisted_state')
if ($persistAlarmStart -lt 0 -or $persistCountStart -lt 0 -or $loadStart -lt 0) {
    throw 'Persistence function boundaries are missing.'
}
$persistBlock = $svc.Substring($persistAlarmStart, $loadStart - $persistAlarmStart)
if ($persistBlock -match 'stopwatch') {
    throw 'Stopwatch state must never be persisted to NVS.'
}

# One-shot semantics: ringing deletes the slot immediately and persists.
$ringStart = $svc.IndexOf('static void start_ringing')
$pollStart = $svc.IndexOf('void timer_service_poll')
if ($ringStart -lt 0 -or $pollStart -lt 0) { throw 'start_ringing/poll boundaries are missing.' }
$ringBlock = $svc.Substring($ringStart, $pollStart - $ringStart)
if ($ringBlock -notmatch 'Delete one-shot sources immediately: the alarm/countdown is consumed') {
    throw 'A ringing timer must delete its one-shot source immediately.'
}
if ($ringBlock -notmatch 's_alarms\[slot - TIMER_SERVICE_SLOT_ALARM0\]\.valid = false' -or
    $ringBlock -notmatch 'persist_alarms\(\);' -or
    $ringBlock -notmatch 's_countdowns\[slot - TIMER_SERVICE_SLOT_COUNTDOWN0\]\.valid = false' -or
    $ringBlock -notmatch 'persist_countdowns\(\);') {
    throw 'Ringing must clear the consumed slot and flush NVS for both slot kinds.'
}

# One chime at a time: a second expiry queues behind the current ring.
if ($ringBlock -notmatch 's_ring_pending = true' -or
    $ringBlock -notmatch 's_pending_slot = slot' -or
    $ringBlock -notmatch 'One chime at a time: queue a second expiry while the first rings') {
    throw 'A second timer expiry must queue behind the current ring instead of overlapping audio.'
}

# Ring heartbeat and 30 s auto-stop inside poll.
$pollBlock = $svc.Substring($pollStart)
if ($pollBlock -notmatch 'TIMER_SERVICE_RING_TIMEOUT_MS \* 1000LL' -or
    $pollBlock -notmatch 'ring auto-stopped') {
    throw 'poll must auto-stop the ring after the 30 s timeout.'
}
if ($pollBlock -notmatch 'TIMER_SERVICE_RING_HEARTBEAT_MS \* 1000LL' -or
    $pollBlock -notmatch 'notify_event\(TIMER_SERVICE_EVENT_RING, s_ringing_slot\);') {
    throw 'poll must emit a RING heartbeat every 2 s while ringing.'
}
if ($pollBlock -notmatch 'if \(!s_ringing && s_ring_pending\) \{') {
    throw 'A queued ring must start as soon as the current ring ends (auto-stop handoff).'
}

# Countdown ticks use real wall-clock elapsed seconds.
if ($pollBlock -notmatch 'Real elapsed seconds since the previous poll' -or
    $pollBlock -notmatch 'elapsed = now - s_last_poll_time' -or
    $pollBlock -notmatch 'remaining_seconds -= \(int32_t\)elapsed') {
    throw 'Countdown progress must follow real elapsed time, not fixed 1 s decrements.'
}

# NVS wear limiting: countdown progress flushes every N ticks.
if ($pollBlock -notmatch 's_persist_tick >= TIMER_SERVICE_COUNTDOWN_PERSIST_TICKS' -or
    $pollBlock -notmatch 'persist_countdowns\(\);') {
    throw 'Countdown progress must flush to NVS every 30 ticks to limit flash wear.'
}

# User stop clears both the current ring and any queued ring.
$stopStart = $svc.IndexOf('void timer_service_stop_ringing')
$stopBlock = $svc.Substring($stopStart, $pollStart - $stopStart)
if ($stopBlock -notmatch 's_ringing = false' -or
    $stopBlock -notmatch 's_ring_pending = false' -or
    $stopBlock -notmatch 'notify_event\(TIMER_SERVICE_EVENT_STOPPED, s_ringing_slot\);') {
    throw 'User stop must clear ringing + pending and emit a STOPPED event.'
}

# Alarm clock gate: needs a synchronized SNTP clock and rolls to tomorrow.
$alarmStart = $svc.IndexOf('timer_service_cmd_result_t timer_service_set_alarm')
$countStart = $svc.IndexOf('timer_service_cmd_result_t timer_service_set_countdown')
$alarmBlock = $svc.Substring($alarmStart, $countStart - $alarmStart)
if ($alarmBlock -notmatch 'timer_service_is_time_synced' -or
    $alarmBlock -notmatch 'mktime\(&local\)' -or
    $alarmBlock -notmatch 'target \+= 24 \* 3600' -or
    $alarmBlock -notmatch 'all %u slots in use') {
    throw 'Alarm set must require a synced clock, roll to tomorrow when passed, and reject when full.'
}
if ($svc -notmatch 'time\(NULL\) > TIMER_TIME_SYNCED_EPOCH') {
    throw 'The synced-clock gate must compare against the SNTP convergence epoch.'
}

# Restore: persisted slots must be reloaded on init.
if ($svc -notmatch 'alarm restored: slot=' -or
    $svc -notmatch 'countdown restored: slot=' -or
    $svc -notmatch 'load_persisted_state\(\);') {
    throw 'init must restore persisted alarms and countdowns.'
}
if ($svc -notmatch 'timer_load_task' -or
    $svc -notmatch 'MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT' -or
    $svc -notmatch 'timer_load_done') {
    throw 'Persisted timer state must be loaded on an internal-RAM stack task.'
}

Write-Output 'Timer service contract passed.'
